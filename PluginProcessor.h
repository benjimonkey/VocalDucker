#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <vector>
#include "SpectralDucker.h"
#include "BandDsp.h"

/** Lock-free audio-thread -> GUI sample transport for the spectrum display. */
class ScopeFifo
{
public:
    static constexpr int capacity = 16384;

    void push (const float* src, int n) noexcept
    {
        int s1, z1, s2, z2;
        fifo.prepareToWrite (n, s1, z1, s2, z2);
        if (z1 > 0) std::copy (src, src + z1, data.begin() + s1);
        if (z2 > 0) std::copy (src + z1, src + z1 + z2, data.begin() + s2);
        fifo.finishedWrite (z1 + z2);
    }

    int pull (float* dest, int maxToRead) noexcept
    {
        int s1, z1, s2, z2;
        fifo.prepareToRead (maxToRead, s1, z1, s2, z2);
        if (z1 > 0) std::copy (data.begin() + s1, data.begin() + s1 + z1, dest);
        if (z2 > 0) std::copy (data.begin() + s2, data.begin() + s2 + z2, dest + z1);
        fifo.finishedRead (z1 + z2);
        return z1 + z2;
    }

private:
    juce::AbstractFifo fifo { capacity };
    std::array<float, capacity> data {};
};

class VocalMixerAudioProcessor : public juce::AudioProcessor
{
public:
    VocalMixerAudioProcessor();
    ~VocalMixerAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "VocalMixer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;

    // Display plumbing (audio thread writes, editor reads)
    ScopeFifo vocalScope, instScope;
    std::atomic<float> gateGainMeter { 0.0f };
    std::atomic<float> duckMeterDb { 0.0f };       // Band mode: current instrument-band cut in dB
    std::atomic<double> currentSampleRate { 44100.0 };

private:
    void resetDspState();

    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pGateThresh = nullptr;
    std::atomic<float>* pGateAttack = nullptr;
    std::atomic<float>* pGateDecay = nullptr;
    std::atomic<float>* pBinThresh = nullptr;
    std::atomic<float>* pDuckAmount = nullptr;
    std::atomic<float>* pMaxCut = nullptr;
    std::atomic<float>* pVocalLevel = nullptr;
    std::atomic<float>* pInstLevel = nullptr;
    std::atomic<float>* pBandLow = nullptr;
    std::atomic<float>* pBandHigh = nullptr;
    std::atomic<float>* pBandThresh = nullptr;
    std::atomic<float>* pCompThresh = nullptr;
    std::atomic<float>* pCompRatio = nullptr;
    std::atomic<float>* pCompAttack = nullptr;
    std::atomic<float>* pCompRelease = nullptr;

    SpectralDucker ducker;

    double sampleRate = 44100.0;
    int lastMode = -1;

    // Noise gate state
    float gateEnv = 0.0f, gateGain = 0.0f;
    bool gateOpen = false;

    // Spectral mode: vocal delay line (compensates the STFT latency of the instrument path)
    std::array<std::vector<float>, 2> vocalDelay;
    int delayPos = 0;

    // Band mode
    BandPassCoeffs bpCoeffs;
    std::array<BandPassChannel, 2> vocalBp;            // vocal only: the instrument channel is not filtered
    Compressor compressor;
    float bandEnv = 0.0f;                              // smoothed power of the band-passed vocal
    std::array<std::vector<float>, 2> bandOutDelay;    // keeps latency identical in both modes
    int outDelayPos = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> vocalGainSmoothed, instGainSmoothed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalMixerAudioProcessor)
};
