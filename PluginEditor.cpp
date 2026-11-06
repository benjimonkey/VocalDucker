#include "PluginEditor.h"

using namespace juce;

namespace
{
    constexpr int kCell = 74;          // width of one knob cell
    constexpr int kPad = 8;            // padding inside a block
    constexpr int kTagW = 66;          // width of the input tags
    constexpr int kLaneH = 150;        // height of a knob block
    constexpr int kGapV = 44;          // vertical gap between the two lanes
    constexpr int kTextBlockH = 80;    // height of blocks without knobs
    constexpr int kInvertRoom = 28;    // space reserved at the bottom of the bin mask block for the INVERT button

    const Colour vocalColour   (0xffff5a50);
    const Colour instColour    (0xff4a90ff);
    const Colour neutralColour (0xff9aa3ad);
    const Colour controlColour (0xffe8a33d);

    float freqToXLog (float freq, Rectangle<float> area)
    {
        const float lo = std::log10 (20.0f), hi = std::log10 (20000.0f);
        const float t = (std::log10 (jlimit (20.0f, 20000.0f, freq)) - lo) / (hi - lo);
        return area.getX() + t * area.getWidth();
    }

    void drawFlowArrow (Graphics& g, Point<float> a, Point<float> b, Colour c, bool dashed)
    {
        const float len = a.getDistanceFrom (b);
        if (len < 4.0f) return;

        const Point<float> u ((b.x - a.x) / len, (b.y - a.y) / len);
        const float headLen = 9.0f, headW = 5.0f;
        const Point<float> lineEnd (b.x - u.x * headLen, b.y - u.y * headLen);
        const Point<float> n (-u.y, u.x);

        g.setColour (c);

        Path line;
        line.startNewSubPath (a);
        line.lineTo (lineEnd);

        if (dashed)
        {
            const float dashes[] = { 5.0f, 4.0f };
            Path dashedPath;
            PathStrokeType (1.5f).createDashedStroke (dashedPath, line, dashes, 2);
            g.fillPath (dashedPath);
        }
        else
        {
            g.strokePath (line, PathStrokeType (1.5f));
        }

        Path head;
        head.addTriangle (b.x, b.y,
                          lineEnd.x + n.x * headW, lineEnd.y + n.y * headW,
                          lineEnd.x - n.x * headW, lineEnd.y - n.y * headW);
        g.fillPath (head);
    }
}

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
    return freqToXLog (freq, area);
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

    // Band-pass region (used in both modes)
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

    // Duck-trace scale: 0 dB at the top of the plot, -range dB at the bottom
    const bool binMode = proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;   // "BAND" button
    const float gainRange = binMode ? 20.0f : 36.0f;

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

        // Amber trace: how each frequency is being ducked right now
        const float sr = (float) proc.currentSampleRate.load();
        const auto& gains = proc.getBinGainDisplay();
        Path trace;
        bool started = false;

        for (int k = 1; k < SpectralDucker::numBins; ++k)
        {
            const float f = (float) k * sr / (float) SpectralDucker::fftSize;
            if (f < 20.0f) continue;
            if (f > 20000.0f) break;

            const float gDb = jlimit (-gainRange, 0.0f, gains[(size_t) k].load (std::memory_order_relaxed));
            const float x = freqToX (f, area);
            const float y = jmap (gDb, -gainRange, 0.0f, area.getBottom(), area.getY());

            if (! started) { trace.startNewSubPath (x, y); started = true; }
            else           trace.lineTo (x, y);
        }

        g.setColour (controlColour);
        g.strokePath (trace, PathStrokeType (2.0f));
    }

    // Right-hand scale for the duck trace
    g.setColour (controlColour.withAlpha (0.85f));
    g.setFont (11.0f);
    for (float frac : { 0.0f, 0.5f, 1.0f })
    {
        const float db = -gainRange * frac;
        const float y = jmap (db, -gainRange, 0.0f, area.getBottom(), area.getY());
        g.drawText (String ((int) db), (int) area.getRight() + 2, (int) y - 7, 32, 14, Justification::centredLeft, false);
    }

    // Legend (right to left: gate light, instruments, vocals, duck trace) - spaced so nothing overlaps
    const int w = getWidth();
    g.setFont (12.0f);

    const float gate = proc.gateGainMeter.load();
    g.setColour (Colours::limegreen.withAlpha (0.15f + 0.85f * gate));
    g.fillEllipse ((float) w - 24.0f, 11.0f, 10.0f, 10.0f);
    g.setColour (Colours::white.withAlpha (0.6f));
    g.drawText ("GATE", w - 70, 8, 42, 16, Justification::centredRight, false);

    g.setColour (Colour (0xff3b8cff));
    g.drawText ("Instruments", w - 174, 8, 90, 16, Justification::centredRight, false);
    g.setColour (Colour (0xffff3b3b));
    g.drawText ("Vocals", w - 244, 8, 60, 16, Justification::centredRight, false);
    g.setColour (controlColour);
    g.drawText ("Duck gain (dB)", w - 354, 8, 100, 16, Justification::centredRight, false);

    if (! binMode)
    {
        g.setColour (controlColour);
        g.drawText ("Band duck: -" + String (proc.duckMeterDb.load(), 1) + " dB",
                    Rectangle<int> (40, 8, 200, 16), Justification::centredLeft, false);
    }
}

//==============================================================================
// BinGainDisplay
//==============================================================================
void BinGainDisplay::paint (Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Colour (0xff101418));
    g.fillRoundedRectangle (bounds, 6.0f);

    auto area = bounds.reduced (34.0f, 16.0f);
    area.removeFromBottom (6.0f);

    constexpr float rangeDb = 20.0f;
    auto yOf = [&] (float db)
    {
        return jmap (jlimit (-rangeDb, 0.0f, db), -rangeDb, 0.0f, area.getBottom(), area.getY());
    };

    // Grid
    g.setFont (11.0f);
    for (float db : { 0.0f, -5.0f, -10.0f, -15.0f, -20.0f })
    {
        const float y = yOf (db);
        g.setColour (Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        g.setColour (Colours::white.withAlpha (0.45f));
        g.drawText (String ((int) db), Rectangle<float> (0.0f, y - 7.0f, 30.0f, 14.0f), Justification::centredRight);
    }

    for (float f : { 100.0f, 1000.0f, 10000.0f })
    {
        const float x = freqToXLog (f, area);
        g.setColour (Colours::white.withAlpha (0.08f));
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        g.setColour (Colours::white.withAlpha (0.45f));
        const String txt = f >= 1000.0f ? String ((int) (f / 1000.0f)) + "k" : String ((int) f);
        g.drawText (txt, Rectangle<float> (x - 20.0f, area.getBottom() - 1.0f, 40.0f, 14.0f), Justification::centred);
    }

    // Bin gains: the area between 0 dB and the gain curve is the amount of ducking
    const float sr = (float) proc.currentSampleRate.load();
    const auto& gains = proc.getBinGainDisplay();

    Path curve;
    bool started = false;
    float firstX = 0.0f, lastX = 0.0f;

    for (int k = 1; k < SpectralDucker::numBins; ++k)
    {
        const float f = (float) k * sr / (float) SpectralDucker::fftSize;
        if (f < 20.0f) continue;
        if (f > 20000.0f) break;

        const float x = freqToXLog (f, area);
        const float y = yOf (gains[(size_t) k].load (std::memory_order_relaxed));

        if (! started) { curve.startNewSubPath (x, y); firstX = x; started = true; }
        else           curve.lineTo (x, y);
        lastX = x;
    }

    if (started)
    {
        Path fill (curve);
        fill.lineTo (lastX, yOf (0.0f));
        fill.lineTo (firstX, yOf (0.0f));
        fill.closeSubPath();

        g.setColour (controlColour.withAlpha (0.25f));
        g.fillPath (fill);
        g.setColour (controlColour);
        g.strokePath (curve, PathStrokeType (1.8f));
    }

    g.setColour (Colours::white.withAlpha (0.6f));
    g.setFont (12.0f);
    g.drawText ("BIN GAINS (dB)", 40, 1, 200, 14, Justification::centredLeft, false);
}

//==============================================================================
// Editor
//==============================================================================
VocalMixerAudioProcessorEditor::VocalMixerAudioProcessorEditor (VocalMixerAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), spectrum (p), binGains (p)
{
    addAndMakeVisible (spectrum);
    addChildComponent (binGains);

    initKnob (lowCut,      "bandLow",     "Low Cut");
    initKnob (highCut,     "bandHigh",    "High Cut");
    initKnob (gateThresh,  "gateThresh",  "Threshold");
    initKnob (gateAttack,  "gateAttack",  "Attack");
    initKnob (gateDecay,   "gateDecay",   "Decay");
    initKnob (binThresh,   "binThresh",   "Bin Thresh");
    initKnob (binMaxCut,   "binMaxCut",   "Max Cut");
    initKnob (compThresh,  "compThresh",  "Threshold");
    initKnob (compRatio,   "compRatio",   "Ratio");
    initKnob (compAttack,  "compAttack",  "Attack");
    initKnob (compRelease, "compRelease", "Release");
    initKnob (bandThresh,  "bandThresh",  "Band Thresh");
    initKnob (duckAmount,  "duckAmount",  "Duck Amount");
    initKnob (maxCut,      "maxCut",      "Max Cut");
    initKnob (vocalLevel,  "vocalLevel",  "Vocal Gain");
    initKnob (instLevel,   "instLevel",   "Inst Gain");

    // Bin mask INVERT button
    invertButton.setClickingTogglesState (true);
    invertButton.setColour (TextButton::buttonColourId, Colour (0xff2b323b));
    invertButton.setColour (TextButton::buttonOnColourId, controlColour);
    invertButton.setColour (TextButton::textColourOffId, Colours::white.withAlpha (0.8f));
    invertButton.setColour (TextButton::textColourOnId, Colour (0xff1a1f26));
    addChildComponent (invertButton);
    invertAttachment = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "binInvert", invertButton);

    // Mode switch: a two-position segmented switch bound to the "mode" parameter
    for (auto* b : { &spectralButton, &bandButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (1001);
        b->setColour (TextButton::buttonColourId, Colour (0xff2b323b));
        b->setColour (TextButton::buttonOnColourId, controlColour);
        b->setColour (TextButton::textColourOffId, Colours::white.withAlpha (0.8f));
        b->setColour (TextButton::textColourOnId, Colour (0xff1a1f26));
        b->onClick = [this] { modeAttachment->setValueAsCompleteGesture (bandButton.getToggleState() ? 1.0f : 0.0f); };
        addAndMakeVisible (*b);
    }
    spectralButton.setConnectedEdges (Button::ConnectedOnRight);
    bandButton.setConnectedEdges (Button::ConnectedOnLeft);

    modeAttachment = std::make_unique<ParameterAttachment> (*proc.apvts.getParameter ("mode"),
        [this] (float v)
        {
            const bool band = v > 0.5f;
            spectralButton.setToggleState (! band, dontSendNotification);
            bandButton.setToggleState (band, dontSendNotification);
            resized();
            repaint();
        });
    modeAttachment->sendInitialUpdate();

    setSize (1000, 660);
}

bool VocalMixerAudioProcessorEditor::isBandMode() const
{
    return proc.apvts.getRawParameterValue ("mode")->load() > 0.5f;
}

void VocalMixerAudioProcessorEditor::initKnob (Knob& k, const String& paramId, const String& name)
{
    addChildComponent (k.slider);
    addChildComponent (k.label);

    k.slider.setTextBoxStyle (Slider::TextBoxBelow, false, 70, 16);
    k.slider.setColour (Slider::rotarySliderFillColourId, controlColour);
    k.slider.setColour (Slider::thumbColourId, Colours::white);

    k.label.setText (name, dontSendNotification);
    k.label.setFont (Font (FontOptions (12.0f)));
    k.label.setJustificationType (Justification::centred);
    k.label.setColour (Label::textColourId, Colours::white.withAlpha (0.85f));

    k.attachment = std::make_unique<AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, paramId, k.slider);
}

void VocalMixerAudioProcessorEditor::layoutKnobs (const Block& b, int extraBottom)
{
    const int n = (int) b.knobs.size();
    int x = b.bounds.getX() + (b.bounds.getWidth() - n * kCell) / 2;

    for (auto* k : b.knobs)
    {
        k->label.setBounds (x, b.bounds.getY() + 42, kCell, 16);
        k->slider.setBounds (x, b.bounds.getY() + 58, kCell, kLaneH - 62 - extraBottom);
        k->label.setVisible (true);
        k->slider.setVisible (true);
        x += kCell;
    }
}

void VocalMixerAudioProcessorEditor::resized()
{
    // The "BAND" button selects the FFT bin-mask layout; the "SPECTRAL" button selects the band-power layout
    const bool binLayout = isBandMode();

    auto area = getLocalBounds();
    area.removeFromTop (44);

    spectralButton.setBounds (150, 9, 96, 26);
    bandButton.setBounds (246, 9, 96, 26);

    // Displays: the spectrum, plus the bin gain display underneath in the bin-mask layout
    auto displays = area.removeFromTop (240).reduced (14, 0);
    if (binLayout)
    {
        spectrum.setBounds (displays.removeFromTop (150));
        displays.removeFromTop (6);
        binGains.setBounds (displays);
        binGains.setVisible (true);
    }
    else
    {
        spectrum.setBounds (displays);
        binGains.setVisible (false);
    }

    area.removeFromTop (10);
    const auto lanes = area.reduced (14, 0);

    const int laneAy = lanes.getY();
    const int laneBy = laneAy + kLaneH + kGapV;
    const int cyA = laneAy + kLaneH / 2;
    const int cyB = laneBy + kLaneH / 2;
    const int outW = kCell + 2 * kPad;
    auto knobsWidth = [] (int n) { return n * kCell + 2 * kPad; };

    // Hide every control; the blocks of the current layout switch theirs back on
    for (auto* k : { &lowCut, &highCut, &gateThresh, &gateAttack, &gateDecay, &binThresh, &binMaxCut, &compThresh,
                     &compRatio, &compAttack, &compRelease, &bandThresh, &duckAmount, &maxCut, &vocalLevel, &instLevel })
    {
        k->slider.setVisible (false);
        k->label.setVisible (false);
    }
    invertButton.setVisible (false);

    blocks.clear();
    arrows.clear();

    auto distribute = [&] (std::initializer_list<int> widths)
    {
        std::vector<int> xs;
        int total = 0;
        for (int w : widths) total += w;
        const int gap = (lanes.getWidth() - total) / ((int) widths.size() - 1);
        int x = lanes.getX();
        for (int w : widths) { xs.push_back (x); x += w + gap; }
        return xs;
    };

    auto make = [&] (int x, int w, int laneY, const String& title, const String& sub,
                     Colour c, std::vector<Knob*> knobs, int extraBottom) -> int
    {
        Block b;
        b.title = title;
        b.subtitle = sub;
        b.colour = c;
        b.knobs = std::move (knobs);
        b.textOnly = b.knobs.empty();
        const int h = b.textOnly ? kTextBlockH : kLaneH;
        b.bounds = Rectangle<int> (x, laneY + (kLaneH - h) / 2, w, h);
        blocks.push_back (std::move (b));
        layoutKnobs (blocks.back(), extraBottom);
        return (int) blocks.size() - 1;
    };

    auto flow = [&] (int from, int to, int cy)
    {
        FlowArrow a;
        a.from = Point<float> ((float) blocks[(size_t) from].bounds.getRight() + 3.0f, (float) cy);
        a.to   = Point<float> ((float) blocks[(size_t) to].bounds.getX() - 3.0f, (float) cy);
        arrows.push_back (a);
    };

    auto control = [&] (int from, int to, const String& label)
    {
        const auto& a = blocks[(size_t) from].bounds;
        const auto& b = blocks[(size_t) to].bounds;
        FlowArrow f;
        f.from = Point<float> ((float) a.getCentreX(), (float) a.getBottom() + 2.0f);
        f.to   = Point<float> ((float) a.getCentreX(), (float) b.getY() - 2.0f);
        f.control = true;
        f.label = label;
        arrows.push_back (f);
    };

    auto makeOutput = [&] (int x) -> int
    {
        Block b;
        b.title = "OUTPUT";
        b.subtitle = "gain, then sum";
        b.note = "sum to\nstereo out";
        b.colour = neutralColour;
        b.bounds = Rectangle<int> (x, laneAy, outW, laneBy + kLaneH - laneAy);
        blocks.push_back (std::move (b));

        auto placeKnob = [&] (Knob& k, int laneY)
        {
            const int kx = x + (outW - kCell) / 2;
            k.label.setBounds (kx, laneY + 42, kCell, 16);
            k.slider.setBounds (kx, laneY + 58, kCell, kLaneH - 62);
            k.label.setVisible (true);
            k.slider.setVisible (true);
        };
        placeKnob (vocalLevel, laneAy);   // vocal lane
        placeKnob (instLevel,  laneBy);   // instrument lane

        return (int) blocks.size() - 1;
    };

    if (binLayout)
    {
        //----------------------------------------------------------------------
        // "BAND": Vocal in > Band-pass > Noise gate > Bin mask (+INVERT)  (control) FFT duck < Inst in
        //----------------------------------------------------------------------
        const int wBpf = knobsWidth (2), wGate = knobsWidth (3), wMask = knobsWidth (1);
        const auto xs = distribute ({ kTagW, wBpf, wGate, wMask, outW });

        const int tagV = make (xs[0], kTagW, laneAy, "VOCAL IN", "input 2", neutralColour, {}, 0);
        const int bpf  = make (xs[1], wBpf,  laneAy, "BAND-PASS", "before gate", vocalColour, { &lowCut, &highCut }, 0);
        const int gate = make (xs[2], wGate, laneAy, "NOISE GATE", "vocal channel", vocalColour, { &gateThresh, &gateAttack, &gateDecay }, 0);
        const int mask = make (xs[3], wMask, laneAy, "BIN MASK", "bins in use", vocalColour, { &binThresh }, kInvertRoom);

        const int tagI = make (xs[0], kTagW, laneBy, "INST. IN", "input 1", neutralColour, {}, 0);
        const int duck = make (xs[2], xs[3] + wMask - xs[2], laneBy, "FFT DUCK",
                               String (SpectralDucker::fftSize) + "-pt FFT, within the band-pass range",
                               instColour, { &duckAmount, &binMaxCut }, 0);

        const int out = makeOutput (xs[4]);

        flow (tagV, bpf, cyA);  flow (bpf, gate, cyA);  flow (gate, mask, cyA);  flow (mask, out, cyA);
        flow (tagI, duck, cyB); flow (duck, out, cyB);
        control (mask, duck, "bin gains");

        const auto& mb = blocks[(size_t) mask].bounds;
        invertButton.setBounds (mb.getCentreX() - 35, mb.getBottom() - kInvertRoom, 70, 22);
        invertButton.setVisible (true);
    }
    else
    {
        //----------------------------------------------------------------------
        // "SPECTRAL": Vocal in > Noise gate > Band-pass > Compressor (tap: band power) > Gain > Out
        //             Inst in > Duck (only the band-pass range; driven by the band power tap)
        //----------------------------------------------------------------------
        const int wGate = knobsWidth (3), wBpf = knobsWidth (2), wComp = knobsWidth (4);
        const auto xs = distribute ({ kTagW, wGate, wBpf, wComp, outW });

        const int tagV = make (xs[0], kTagW, laneAy, "VOCAL IN", "input 2", neutralColour, {}, 0);
        const int gate = make (xs[1], wGate, laneAy, "NOISE GATE", "vocal channel", vocalColour, { &gateThresh, &gateAttack, &gateDecay }, 0);
        const int bpf  = make (xs[2], wBpf,  laneAy, "BAND-PASS", "after gate", vocalColour, { &lowCut, &highCut }, 0);
        const int comp = make (xs[3], wComp, laneAy, "COMPRESSOR", "post band-pass", vocalColour, { &compThresh, &compRatio, &compAttack, &compRelease }, 0);

        const int tagI = make (xs[0], kTagW, laneBy, "INST. IN", "input 1", neutralColour, {}, 0);
        const int duck = make (xs[3], wComp, laneBy, "DUCK", "only the band-pass range", instColour, { &bandThresh, &duckAmount, &maxCut }, 0);

        const int out = makeOutput (xs[4]);

        flow (tagV, gate, cyA); flow (gate, bpf, cyA); flow (bpf, comp, cyA); flow (comp, out, cyA);
        flow (tagI, duck, cyB); flow (duck, out, cyB);
        control (comp, duck, "band power (post-comp)");
    }

    repaint();
}

void VocalMixerAudioProcessorEditor::paint (Graphics& g)
{
    g.fillAll (Colour (0xff1a1f26));

    // Header
    g.setColour (Colours::white);
    g.setFont (Font (FontOptions (18.0f, Font::bold)));
    g.drawText ("VocalMixer", 14, 8, 130, 28, Justification::centredLeft);

    const double sr = jmax (1.0, proc.currentSampleRate.load());
    const float latencyMs = 1000.0f * (float) SpectralDucker::latencySamples / (float) sr;
    g.setColour (Colours::white.withAlpha (0.55f));
    g.setFont (Font (FontOptions (12.0f)));
    g.drawText ("Latency: " + String (SpectralDucker::latencySamples) + " samples (" + String (latencyMs, 1) + " ms)",
                getWidth() - 300, 8, 286, 28, Justification::centredRight);

    // Blocks
    for (const auto& b : blocks)
    {
        const auto r = b.bounds.toFloat();
        g.setColour (b.colour.withAlpha (0.10f));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (b.colour.withAlpha (0.55f));
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);

        const int titleY = b.textOnly ? b.bounds.getY() + 22 : b.bounds.getY() + 6;

        g.setColour (b.colour.brighter (0.4f));
        g.setFont (Font (FontOptions (12.0f, Font::bold)));
        g.drawText (b.title, b.bounds.getX(), titleY, b.bounds.getWidth(), 16, Justification::centred, false);

        g.setColour (Colours::white.withAlpha (0.5f));
        g.setFont (Font (FontOptions (11.0f)));
        g.drawText (b.subtitle, b.bounds.getX(), titleY + 16, b.bounds.getWidth(), 14, Justification::centred, false);

        if (b.note.isNotEmpty())
        {
            g.setColour (Colours::white.withAlpha (0.55f));
            g.drawFittedText (b.note, b.bounds.getX() + 2, b.bounds.getY() + kLaneH,
                              b.bounds.getWidth() - 4, kGapV, Justification::centred, 2);
        }
    }

    // Arrows and control labels
    for (const auto& a : arrows)
    {
        drawFlowArrow (g, a.from, a.to, a.control ? controlColour : Colours::white.withAlpha (0.55f), a.control);

        if (a.label.isNotEmpty())
        {
            g.setColour (controlColour);
            g.setFont (Font (FontOptions (11.0f)));
            g.drawText (a.label, (int) a.from.x + 8, (int) ((a.from.y + a.to.y) * 0.5f) - 7, 170, 14,
                        Justification::centredLeft, false);
        }
    }
}
