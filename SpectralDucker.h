#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

/**
    Frequency-domain ducker for the instrument channel (512-point STFT, 75 % overlap).

    The per-bin gain applied to the instruments is always limited to the band-pass range:
    `bandMask` holds the magnitude response of the vocal band-pass filter at every bin, and
    the ducking at a bin is scaled by it, so frequencies outside the filter band are never ducked.

    Mode::vocalMask   The (band-passed, gated) vocal is analysed. Bins whose level is above the
                      bin threshold form the mask; instrument bins under the mask are cut by up
                      to maxCutDb. `invert` flips the mask (duck where the vocal is NOT).
    Mode::fixedBand   No vocal analysis. The band selected by the band-pass filter is cut by
                      `bandGain` (a linear gain set per sample from the measured band power).

    The gain of every bin is published in `displayGainDb` for the GUI.

    Latency = fftSize samples (reported to the host by the plugin).
*/
class SpectralDucker
{
public:
    static constexpr int fftOrder = 9;
    static constexpr int fftSize = 1 << fftOrder;   // 512
    static constexpr int hopSize = fftSize / 4;     // 128 (75 % overlap)
    static constexpr int numBins = fftSize / 2 + 1;
    static constexpr int latencySamples = fftSize;

    enum class Mode { vocalMask, fixedBand };

    SpectralDucker() : fft (fftOrder)
    {
        float sumSq = 0.0f;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) fftSize);
            sumSq += window[(size_t) i] * window[(size_t) i];
        }
        outputScale = (float) hopSize / sumSq;   // = 1 / 1.5 for Hann @ 75 % overlap
        bandMask.fill (1.0f);
        prepare (48000.0);
        reset();
    }

    /** Converts the mask smoothing time constants to per-frame coefficients. */
    void prepare (double sampleRate)
    {
        riseCoef = 1.0f - std::exp (-(float) hopSize / (0.004f * (float) sampleRate));   //  4 ms: quick to duck
        fallCoef = 1.0f - std::exp (-(float) hopSize / (0.050f * (float) sampleRate));   // 50 ms: slower release
    }

    void reset()
    {
        for (auto& b : inBuf)  b.fill (0.0f);
        for (auto& b : outBuf) b.fill (0.0f);
        mask.fill (0.0f);
        for (auto& d : displayGainDb) d.store (0.0f, std::memory_order_relaxed);
        writePos = 0;
        hopCounter = 0;
    }

    void setMode (Mode m) noexcept { mode = m; }

    /** vocalMask mode: bin threshold (dB), amount (0..1), deepest cut (dB), invert the mask. */
    void setParameters (float binThresholdDbIn, float amountIn, float maxCutDbIn, bool invertIn)
    {
        binThresholdDb = binThresholdDbIn;
        amount = amountIn;
        minGain = juce::Decibels::decibelsToGain (-maxCutDbIn);
        invert = invertIn;
    }

    /** Magnitude response (0..1) of the band-pass filter at each of the numBins bins. */
    void setBandMask (const float* m) { std::copy (m, m + numBins, bandMask.begin()); }

    /** fixedBand mode: linear gain applied inside the band (1 = no ducking). */
    void setBandGain (float g) noexcept { bandGain = g; }

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

    /** Per-bin gain in dB (<= 0), updated every frame, for the GUI. */
    std::array<std::atomic<float>, numBins> displayGainDb {};

private:
    void gather (const std::array<float, fftSize>& src, std::array<float, fftSize * 2>& dst) const noexcept
    {
        for (int i = 0; i < fftSize; ++i)
            dst[(size_t) i] = src[(size_t) ((writePos + i) & (fftSize - 1))] * window[(size_t) i];

        std::fill (dst.begin() + fftSize, dst.end(), 0.0f);
    }

    void processFrame() noexcept
    {
        gather (inBuf[1], instData[0]);
        gather (inBuf[2], instData[1]);

        fft.performRealOnlyForwardTransform (instData[0].data(), false);
        fft.performRealOnlyForwardTransform (instData[1].data(), false);

        if (mode == Mode::vocalMask)
        {
            gather (inBuf[0], vocalData);
            fft.performRealOnlyForwardTransform (vocalData.data(), false);

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

            // 2) Smooth across frequency and time, optionally invert, limit to the band-pass range
            for (int k = 0; k < numBins; ++k)
            {
                const float a = target[(size_t) std::max (k - 1, 0)];
                const float b = target[(size_t) k];
                const float c = target[(size_t) std::min (k + 1, numBins - 1)];
                const float sm = 0.25f * (a + 2.0f * b + c);

                float& m = mask[(size_t) k];
                m += (sm > m ? riseCoef : fallCoef) * (sm - m);

                const float used = invert ? 1.0f - m : m;
                binGain[(size_t) k] = 1.0f - amount * used * bandMask[(size_t) k] * (1.0f - minGain);
            }
        }
        else
        {
            // Fixed band: duck only the frequencies passed by the band-pass filter
            for (int k = 0; k < numBins; ++k)
                binGain[(size_t) k] = 1.0f - (1.0f - bandGain) * bandMask[(size_t) k];
        }

        // Publish for the GUI
        for (int k = 0; k < numBins; ++k)
            displayGainDb[(size_t) k].store (20.0f * std::log10 (std::max (binGain[(size_t) k], 1.0e-4f)),
                                             std::memory_order_relaxed);

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

    std::array<float, numBins> target {}, mask {}, binGain {}, bandMask {};

    int writePos = 0, hopCounter = 0;

    Mode mode = Mode::vocalMask;
    bool invert = false;
    float binThresholdDb = -55.0f, amount = 0.7f, minGain = 0.25f, gateGain = 0.0f, bandGain = 1.0f;
    float riseCoef = 0.5f, fallCoef = 0.1f;
};
