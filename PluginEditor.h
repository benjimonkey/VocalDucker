#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <vector>
#include "PluginProcessor.h"

/** Live FFT display: vocals in red, instruments in blue. In Band mode the filter band is shaded. */
class SpectrumDisplay : public juce::Component,
                        private juce::Timer
{
public:
    explicit SpectrumDisplay (VocalMixerAudioProcessor& p);
    ~SpectrumDisplay() override { stopTimer(); }

    void paint (juce::Graphics&) override;

private:
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

    void initKnob (Knob& k, const juce::String& paramId, const juce::String& name);
    void updateModeVisibility();
    bool isBandMode() const;

    VocalMixerAudioProcessor& proc;
    SpectrumDisplay spectrum;

    juce::Label modeLabel;
    juce::ComboBox modeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;

    Knob gateThresh, gateAttack, gateDecay;                 // both modes
    Knob binThresh;                                         // Spectral mode
    Knob bandLow, bandHigh, bandThresh;                     // Band mode
    Knob compThresh, compRatio, compAttack, compRelease;    // Band mode
    Knob duckAmount, maxCut;                                // both modes
    Knob vocalLevel;                                        // both modes

    juce::Rectangle<int> gateArea, detectArea, compArea, duckArea, outArea;
    juce::String detectTitle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalMixerAudioProcessorEditor)
};
