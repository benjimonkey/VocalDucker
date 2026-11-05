#include "PluginEditor.h"

using namespace juce;

//==============================================================================
// SpectrumDisplay
//==============================================================================
SpectrumDisplay::SpectrumDisplay (VocalMixerAudioProcessor& p)
    : proc (p)
{
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * (float) i / (float) fftSize);

    vocalDb.fill (floorDb);
    instDb.fill (floorDb);
    pullBuffer.resize ((size_t) ScopeFifo::capacity);

    startTimerHz (30);
}

void SpectrumDisplay::slide (std::array<float, fftSize>& win, const float* in, int n)
{
    if (n >= fftSize)
    {
        std::copy (in + (n - fftSize), in + n, win.begin());
    }
    else
    {
        std::memmove (win.data(), win.data() + n, (size_t) (fftSize - n) * sizeof (float));
        std::copy (in, in + n, win.begin() + (fftSize - n));
    }
}

void SpectrumDisplay::analyse (const std::array<float, fftSize>& win, std::array<float, numBins>& dbOut)
{
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = win[(size_t) i] * window[(size_t) i];
    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);

    fft.performFrequencyOnlyForwardTransform (fftData.data());

    const float ref = (float) fftSize * 0.25f;   // full-scale sine with Hann window
    for (int k = 0; k < numBins; ++k)
    {
        const float db = Decibels::gainToDecibels (fftData[(size_t) k] / ref, floorDb);

        float& d = dbOut[(size_t) k];
        d = db > d ? d + 0.6f * (db - d)          // fast rise
                   : std::max (db, d - 1.5f);     // slow fall
    }
}

void SpectrumDisplay::timerCallback()
{
    const int nv = proc.vocalScope.pull (pullBuffer.data(), (int) pullBuffer.size());
    if (nv > 0) slide (vocalWin, pullBuffer.data(), nv);

    const int ni = proc.instScope.pull (pullBuffer.data(), (int) pullBuffer.size());
    if (ni > 0) slide (instWin, pullBuffer.data(), ni);

    idleTicks = (nv == 0 && ni == 0) ? idleTicks + 1 : 0;
    if (idleTicks == 10)    // transport stopped: let the display fall away
    {
        vocalWin.fill (0.0f);
        instWin.fill (0.0f);
    }

    analyse (vocalWin, vocalDb);
    analyse (instWin, instDb);
    repaint();
}

float SpectrumDisplay::freqToX (float freq, Rectangle<float> area) const
{
    const float lo = std::log10 (20.0f), hi = std::log10 (20000.0f);
    const float t = (std::log10 (jlimit (20.0f, 20000.0f, freq)) - lo) / (hi - lo);
    return area.getX() + t * area.getWidth();
}

Path SpectrumDisplay::makePath (const std::array<float, numBins>& db, Rectangle<float> area) const
{
    const float sr = (float) proc.currentSampleRate.load();
    Path p;
    bool started = false;

    for (int k = 1; k < numBins; ++k)
    {
        const float f = (float) k * sr / (float) fftSize;
        if (f < 20.0f) continue;
        if (f > 20000.0f) break;

        const float x = freqToX (f, area);
        const float y = jmap (db[(size_t) k], floorDb, 0.0f, area.getBottom(), area.getY());

        if (! started) { p.startNewSubPath (x, y); started = true; }
        else           p.lineTo (x, y);
    }
    return p;
}

void SpectrumDisplay::paint (Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Colour (0xff101418));
    g.fillRoundedRectangle (bounds, 6.0f);

    auto area = bounds.reduced (34.0f, 14.0f);
    area.removeFromBottom (6.0f);

    // Grid
    g.setFont (11.0f);
    for (float db = 0.0f; db >= -100.0f; db -= 20.0f)
    {
        const float y = jmap (db, floorDb, 0.0f, area.getBottom(), area.getY());
        g.setColour (Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        g.setColour (Colours::white.withAlpha (0.45f));
        g.drawText (String ((int) db), Rectangle<float> (0.0f, y - 7.0f, 30.0f, 14.0f), Justification::centredRight);
    }

    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = freqToX (f, area);
        g.setColour (Colours::white.withAlpha (0.08f));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (Colours::white.withAlpha (0.45f));
        const String txt = f >= 1000.0f ? String ((int) (f / 1000.0f)) + "k" : String ((int) f);
        g.drawText (txt, Rectangle<float> (x - 20.0f, area.getBottom() - 1.0f, 40.0f, 14.0f), Justification::centred);
    }

    const bool bandMode = proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;

    // Band-pass region (Band mode)
    if (bandMode)
    {
        const float lo = proc.apvts.getRawParameterValue ("bandLow")->load();
        const float hi = jmax (proc.apvts.getRawParameterValue ("bandHigh")->load(), lo * 1.2f);
        const float x1 = freqToX (lo, area), x2 = freqToX (hi, area);

        g.setColour (Colours::white.withAlpha (0.07f));
        g.fillRect (Rectangle<float> (x1, area.getY(), x2 - x1, area.getHeight()));
        g.setColour (Colours::white.withAlpha (0.35f));
        g.drawVerticalLine ((int) x1, area.getY(), area.getBottom());
        g.drawVerticalLine ((int) x2, area.getY(), area.getBottom());
    }

    // Spectra (instruments underneath, vocals on top)
    {
        Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (area.toNearestInt());

        auto drawSpectrum = [&] (const std::array<float, numBins>& db, Colour c)
        {
            auto line = makePath (db, area);
            if (line.isEmpty()) return;

            Path fill (line);
            fill.lineTo (area.getRight(), area.getBottom());
            fill.lineTo (area.getX(), area.getBottom());
            fill.closeSubPath();

            g.setColour (c.withAlpha (0.18f));
            g.fillPath (fill);
            g.setColour (c);
            g.strokePath (line, PathStrokeType (1.6f));
        };

        drawSpectrum (instDb,  Colour (0xff3b8cff));   // blue - instruments
        drawSpectrum (vocalDb, Colour (0xffff3b3b));   // red  - vocals
    }

    // Legend, gate light, duck readout
    g.setFont (13.0f);
    g.setColour (Colour (0xffff3b3b));
    g.drawText ("Vocals", Rectangle<int> (getWidth() - 220, 8, 60, 16), Justification::centredRight);
    g.setColour (Colour (0xff3b8cff));
    g.drawText ("Instruments", Rectangle<int> (getWidth() - 150, 8, 90, 16), Justification::centredRight);

    const float gate = proc.gateGainMeter.load();
    g.setColour (Colours::limegreen.withAlpha (0.15f + 0.85f * gate));
    g.fillEllipse ((float) getWidth() - 46.0f, 11.0f, 10.0f, 10.0f);
    g.setColour (Colours::white.withAlpha (0.6f));
    g.drawText ("GATE", Rectangle<int> (getWidth() - 82, 8, 32, 16), Justification::centredRight, false);

    if (bandMode)
    {
        g.setColour (Colour (0xffe8a33d));
        g.drawText ("Band duck: -" + String (proc.duckMeterDb.load(), 1) + " dB",
                    Rectangle<int> (40, 8, 200, 16), Justification::centredLeft);
    }
}

//==============================================================================
// Editor
//==============================================================================
VocalMixerAudioProcessorEditor::VocalMixerAudioProcessorEditor (VocalMixerAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), spectrum (p)
{
    addAndMakeVisible (spectrum);

    modeLabel.setText ("Mode", dontSendNotification);
    modeLabel.setColour (Label::textColourId, Colours::white.withAlpha (0.85f));
    addAndMakeVisible (modeLabel);

    modeBox.addItemList (StringArray { "Spectral", "Band" }, 1);
    addAndMakeVisible (modeBox);
    modeAttachment = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "mode", modeBox);
    modeBox.onChange = [this] { updateModeVisibility(); };

    initKnob (gateThresh, "gateThresh", "Threshold");
    initKnob (gateAttack, "gateAttack", "Attack");
    initKnob (gateDecay,  "gateDecay",  "Decay");

    initKnob (binThresh,  "binThresh",  "Bin Threshold");

    initKnob (bandLow,    "bandLow",    "BPF Low Cut");
    initKnob (bandHigh,   "bandHigh",   "BPF High Cut");
    initKnob (bandThresh, "bandThresh", "Band Threshold");

    initKnob (compThresh,  "compThresh",  "Threshold");
    initKnob (compRatio,   "compRatio",   "Ratio");
    initKnob (compAttack,  "compAttack",  "Attack");
    initKnob (compRelease, "compRelease", "Release");

    initKnob (duckAmount, "duckAmount", "Duck Amount");
    initKnob (maxCut,     "maxCut",     "Max Cut");

    initKnob (vocalLevel, "vocalLevel", "Vocal Gain");

    updateModeVisibility();
    setSize (800, 650);
}

bool VocalMixerAudioProcessorEditor::isBandMode() const
{
    return modeBox.getSelectedItemIndex() == 1;
}

void VocalMixerAudioProcessorEditor::updateModeVisibility()
{
    const bool band = isBandMode();

    binThresh.slider.setVisible (! band);
    binThresh.label.setVisible (! band);

    for (auto* k : { &bandLow, &bandHigh, &bandThresh, &compThresh, &compRatio, &compAttack, &compRelease })
    {
        k->slider.setVisible (band);
        k->label.setVisible (band);
    }

    resized();
    repaint();
}

void VocalMixerAudioProcessorEditor::initKnob (Knob& k, const String& paramId, const String& name)
{
    addAndMakeVisible (k.slider);
    addAndMakeVisible (k.label);

    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 78, 18);
    k.slider.setColour (Slider::rotarySliderFillColourId, Colour (0xffe8a33d));
    k.slider.setColour (Slider::thumbColourId, Colours::white);

    k.label.setText (name, dontSendNotification);
    k.label.setJustificationType (Justification::centred);
    k.label.setColour (Label::textColourId, Colours::white.withAlpha (0.85f));

    k.attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, paramId, k.slider);
}

void VocalMixerAudioProcessorEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1a1f26));

    auto drawGroup = [&] (Rectangle<int> r, const String& title)
    {
        if (r.isEmpty()) return;

        g.setColour (Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (Colours::white.withAlpha (0.55f));
        g.setFont (Font (FontOptions (13.0f, Font::bold)));
        g.drawText (title, r.withHeight (22), Justification::centred);
    };

    drawGroup (gateArea,   "VOCAL NOISE GATE");
    drawGroup (detectArea, detectTitle);
    drawGroup (compArea,   "VOCAL COMPRESSOR (after BPF)");
    drawGroup (duckArea,   "DUCKING");
    drawGroup (outArea,    "VOCAL GAIN");
}

void VocalMixerAudioProcessorEditor::resized()
{
    constexpr int cell = 110;
    const bool band = isBandMode();

    auto area = getLocalBounds().reduced (10);

    auto header = area.removeFromTop (30);
    modeLabel.setBounds (header.removeFromLeft (50));
    modeBox.setBounds (header.removeFromLeft (140).reduced (0, 3));
    area.removeFromTop (4);

    spectrum.setBounds (area.removeFromTop (270));
    area.removeFromTop (10);

    auto row1 = area.removeFromTop (150);
    area.removeFromTop (10);
    auto row2 = area.removeFromTop (150);

    gateArea   = row1.removeFromLeft (cell * 3).reduced (2, 0);
    detectArea = row1.removeFromLeft (cell * (band ? 3 : 1)).reduced (2, 0);
    detectTitle = band ? "BAND-PASS FILTER" : "SPECTRAL";

    duckArea = row2.removeFromLeft (cell * 2).reduced (2, 0);
    outArea  = row2.removeFromLeft (cell).reduced (2, 0);
    compArea = band ? row2.removeFromLeft (cell * 4).reduced (2, 0) : Rectangle<int>();

    auto place = [] (Knob& k, Rectangle<int> r)
    {
        r.removeFromTop (24);   // group title
        k.label.setBounds (r.removeFromTop (18));
        k.slider.setBounds (r);
    };

    auto placeRow = [&] (Rectangle<int> r, std::initializer_list<Knob*> knobs)
    {
        const int w = r.getWidth() / (int) knobs.size();
        for (auto* k : knobs)
            place (*k, r.removeFromLeft (w));
    };

    placeRow (gateArea, { &gateThresh, &gateAttack, &gateDecay });

    if (band)
        placeRow (detectArea, { &bandLow, &bandHigh, &bandThresh });
    else
        placeRow (detectArea, { &binThresh });

    placeRow (duckArea, { &duckAmount, &maxCut });
    placeRow (outArea,  { &vocalLevel });

    if (band)
        placeRow (compArea, { &compThresh, &compRatio, &compAttack, &compRelease });
}
