#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>

/**
    Frequency-domain processor.

    The (already gated) vocal signal is analysed with an STFT. Every FFT bin whose
    level is above the bin threshold is marked as "used by the vocal". The same bins
    of the instrument signal are then attenuated by `amount` (up to `maxCutDb`).
    Where the vocal is absent (gate closed or bins below threshold) the instrument
    bins pass through unchanged.

    Latency = fftSize samples (reported to the host by the plugin).
*/
class SpectralDucker
{
public:
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;   // 2048
    static constexpr int hopSize = fftSize / 4;     // 75 % overlap
    static constexpr int numBins = fftSize / 2 + 1;
    static constexpr int latencySamples = fftSize;

    SpectralDucker() : fft (fftOrder)
    {
        float sumSq = 0.0f;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
            sumSq += window[(size_t) i] * window[(size_t) i];
        }
        outputScale = (float) hopSize / sumSq;   // = 1 / 1.5 for Hann @ 75 % overlap
        reset();
    }

    void reset()
    {
        for (auto& b : inBuf)  b.fill (0.0f);
        for (auto& b : outBuf) b.fill (0.0f);
        mask.fill (0.0f);
        writePos = 0;
        hopCounter = 0;
    }

    void setParameters (float binThresholdDbIn, float amountIn, float maxCutDbIn)
    {
        binThresholdDb = binThresholdDbIn;
        amount = amountIn;
        minGain = juce::Decibels::decibelsToGain (-maxCutDbIn);
    }

    /** Current gain of the vocal noise gate (0..1). Scales the vocal-presence mask. */
    void setGate (float g) noexcept { gateGain = g; }

    void processSample (float vocalMono, float instL, float instR, float& outL, float& outR) noexcept
    {
        inBuf[0][(size_t) writePos] = vocalMono;
        inBuf[1][(size_t) writePos] = instL;
        inBuf[2][(size_t) writePos] = instR;

        outL = outBuf[0][(size_t) writePos];
        outR = outBuf[1][(size_t) writePos];
        outBuf[0][(size_t) writePos] = 0.0f;
        outBuf[1][(size_t) writePos] = 0.0f;

        writePos = (writePos + 1) & (fftSize - 1);

        if (++hopCounter >= hopSize)
        {
            hopCounter = 0;
            processFrame();
        }
    }

private:
    void gather (const std::array<float, fftSize>& src, std::array<float, fftSize * 2>& dst) const noexcept
    {
        for (int i = 0; i < fftSize; ++i)
            dst[(size_t) i] = src[(size_t) ((writePos + i) & (fftSize - 1))] * window[(size_t) i];

        std::fill (dst.begin() + fftSize, dst.end(), 0.0f);
    }

    void processFrame() noexcept
    {
        gather (inBuf[0], vocalData);
        gather (inBuf[1], instData[0]);
        gather (inBuf[2], instData[1]);

        fft.performRealOnlyForwardTransform (vocalData.data(), false);
        fft.performRealOnlyForwardTransform (instData[0].data(), false);
        fft.performRealOnlyForwardTransform (instData[1].data(), false);

        // 1) Which bins does the vocal occupy?
        const float refMag = (float) fftSize * 0.25f;   // magnitude of a full-scale sine with a Hann window
        constexpr float kneeDb = 12.0f;

        for (int k = 0; k < numBins; ++k)
        {
            const float re = vocalData[(size_t) (2 * k)];
            const float im = vocalData[(size_t) (2 * k + 1)];
            const float mag = std::sqrt (re * re + im * im) / refMag;
            const float db = 20.0f * std::log10 (std::max (mag, 1.0e-9f));
            const float presence = juce::jlimit (0.0f, 1.0f, (db - (binThresholdDb - kneeDb * 0.5f)) / kneeDb);
            target[(size_t) k] = presence * gateGain;
        }

        // 2) Smooth across frequency and time, then turn into a per-bin gain
        for (int k = 0; k < numBins; ++k)
        {
            const float a = target[(size_t) std::max (k - 1, 0)];
            const float b = target[(size_t) k];
            const float c = target[(size_t) std::min (k + 1, numBins - 1)];
            const float sm = 0.25f * (a + 2.0f * b + c);

            float& m = mask[(size_t) k];
            m += (sm > m ? 0.7f : 0.25f) * (sm - m);   // quick to duck, slower to release

            binGain[(size_t) k] = 1.0f - amount * m * (1.0f - minGain);
        }

        // 3) Apply to both instrument channels (symmetric spectrum) and resynthesise
        for (auto& data : instData)
        {
            for (int k = 0; k < numBins; ++k)
            {
                const float g = binGain[(size_t) k];
                data[(size_t) (2 * k)] *= g;
                data[(size_t) (2 * k + 1)] *= g;

                if (k > 0 && k < fftSize / 2)
                {
                    const int mk = fftSize - k;
                    data[(size_t) (2 * mk)] *= g;
                    data[(size_t) (2 * mk + 1)] *= g;
                }
            }
        }

        fft.performRealOnlyInverseTransform (instData[0].data());
        fft.performRealOnlyInverseTransform (instData[1].data());

        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < fftSize; ++i)
                outBuf[(size_t) c][(size_t) ((writePos + i) & (fftSize - 1))]
                    += instData[(size_t) c][(size_t) i] * window[(size_t) i] * outputScale;
    }

    juce::dsp::FFT fft;

    std::array<float, fftSize> window {};
    float outputScale = 1.0f;

    std::array<std::array<float, fftSize>, 3> inBuf {};    // vocal mono, inst L, inst R
    std::array<std::array<float, fftSize>, 2> outBuf {};   // inst L, inst R (overlap-add)

    std::array<float, fftSize * 2> vocalData {};
    std::array<std::array<float, fftSize * 2>, 2> instData {};

    std::array<float, numBins> target {}, mask {}, binGain {};

    int writePos = 0, hopCounter = 0;

    float binThresholdDb = -55.0f, amount = 0.7f, minGain = 0.25f, gateGain = 0.0f;
};
