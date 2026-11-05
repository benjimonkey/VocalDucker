#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include "PluginProcessor.h"

/** Live FFT display: vocals in red, instruments in blue. The band-pass region is shaded. */
class SpectrumDisplay : public juce::Component,
                        private juce::Timer
{
public:
    explicit SpectrumDisplay (VocalMixerAudioProcessor& p);
    ~SpectrumDisplay() override { stopTimer(); }

    void paint (juce::Graphics&) override;

private:
    // Display resolution only - independent of the 512-point processing FFT
    static constexpr int fftOrder = 11;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int numBins = fftSize / 2;
    static constexpr float floorDb = -100.0f;

    void timerCallback() override;
    void slide (std::array<float, fftSize>& win, const float* in, int n);
    void analyse (const std::array<float, fftSize>& win, std::array<float, numBins>& dbOut);
    juce::Path makePath (const std::array<float, numBins>& db, juce::Rectangle<float> area) const;
    float freqToX (float freq, juce::Rectangle<float> area) const;

    VocalMixerAudioProcessor& proc;
    juce::dsp::FFT fft { fftOrder };

    std::array<float, fftSize> window {};
    std::array<float, fftSize> vocalWin {}, instWin {};
    std::array<float, fftSize * 2> fftData {};
    std::array<float, numBins> vocalDb {}, instDb {};
    std::vector<float> pullBuffer;
    int idleTicks = 0;
};

/**
    The editor lays the controls out as the signal flow: one lane for the vocal path,
    one for the instrument path, blocks left-to-right in processing order, arrows between them.
*/
class VocalMixerAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit VocalMixerAudioProcessorEditor (VocalMixerAudioProcessor&);
    ~VocalMixerAudioProcessorEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Knob
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    struct Block
    {
        juce::Rectangle<int> bounds;
        juce::String title, subtitle, note;
        juce::Colour colour;
        std::vector<Knob*> knobs;
        bool textOnly = false;
    };

    struct FlowArrow
    {
        juce::Point<float> from, to;
        bool control = false;
        juce::String label;
    };

    void initKnob (Knob& k, const juce::String& paramId, const juce::String& name);
    bool isBandMode() const;
    void layoutKnobs (const Block& b);

    VocalMixerAudioProcessor& proc;
    SpectrumDisplay spectrum;

    juce::TextButton spectralButton { "SPECTRAL" }, bandButton { "BAND" };
    std::unique_ptr<juce::ParameterAttachment> modeAttachment;

    Knob lowCut, highCut;                                   // band-pass (both modes)
    Knob gateThresh, gateAttack, gateDecay;                 // noise gate (both modes)
    Knob binThresh;                                         // Spectral: bin mask
    Knob compThresh, compRatio, compAttack, compRelease;    // Band: compressor
    Knob bandThresh;                                        // Band: duck threshold
    Knob duckAmount, maxCut;                                // ducking (both modes)
    Knob vocalLevel;                                        // output gain stage

    std::vector<Block> blocks;
    std::vector<FlowArrow> arrows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalMixerAudioProcessorEditor)
};
