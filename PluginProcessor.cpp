#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

/*
    MODES (label shown on the GUI -> what it does)

      "Spectral" (parameter index 0, the default):
          vocal -> noise gate -> band-pass -> compressor -> [band power] -> gain
          The band power ducks the instrument channel, but ONLY the frequencies passed by the
          band-pass filter (done with per-bin gains in the 512-pt ducker, so the instruments are
          never filtered).

      "Band" (parameter index 1):
          vocal -> band-pass -> noise gate -> FFT bin mask (optionally inverted)
          Bins used by the vocal duck the same bins of the instruments, limited to the band-pass range.
*/

VocalMixerAudioProcessor::VocalMixerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Instruments", AudioChannelSet::stereo(), true)   // Stereo Input 1
                          .withInput  ("Vocal",       AudioChannelSet::stereo(), true)   // Stereo Input 2
                          .withOutput ("Output",      AudioChannelSet::stereo(), true)), // Stereo Output 1
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pMode        = apvts.getRawParameterValue ("mode");
    pGateThresh  = apvts.getRawParameterValue ("gateThresh");
    pGateAttack  = apvts.getRawParameterValue ("gateAttack");
    pGateDecay   = apvts.getRawParameterValue ("gateDecay");
    pBinThresh   = apvts.getRawParameterValue ("binThresh");
    pBinMaxCut   = apvts.getRawParameterValue ("binMaxCut");
    pBinInvert   = apvts.getRawParameterValue ("binInvert");
    pDuckAmount  = apvts.getRawParameterValue ("duckAmount");
    pMaxCut      = apvts.getRawParameterValue ("maxCut");
    pVocalLevel  = apvts.getRawParameterValue ("vocalLevel");
    pInstLevel   = apvts.getRawParameterValue ("instLevel");
    pBandLow     = apvts.getRawParameterValue ("bandLow");
    pBandHigh    = apvts.getRawParameterValue ("bandHigh");
    pBandThresh  = apvts.getRawParameterValue ("bandThresh");
    pCompThresh  = apvts.getRawParameterValue ("compThresh");
    pCompRatio   = apvts.getRawParameterValue ("compRatio");
    pCompAttack  = apvts.getRawParameterValue ("compAttack");
    pCompRelease = apvts.getRawParameterValue ("compRelease");

    setLatencySamples (SpectralDucker::latencySamples);
}

AudioProcessorValueTreeState::ParameterLayout VocalMixerAudioProcessor::createLayout()
{
    using P = AudioParameterFloat;
    using A = AudioParameterFloatAttributes;

    AudioProcessorValueTreeState::ParameterLayout layout;

    // Index 0 = "Spectral" (default), index 1 = "Band"
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
                                                        StringArray { "Spectral", "Band" }, 0));

    // --- Band-pass filter on the vocal (both modes) ---
    layout.add (std::make_unique<P> (ParameterID { "bandLow", 1 }, "BPF Low Cut",
                                     NormalisableRange<float> (20.0f, 8000.0f, 1.0f, 0.35f), 100.0f, A().withLabel ("Hz")));
    layout.add (std::make_unique<P> (ParameterID { "bandHigh", 1 }, "BPF High Cut",
                                     NormalisableRange<float> (200.0f, 20000.0f, 1.0f, 0.35f), 8000.0f, A().withLabel ("Hz")));

    // --- Noise gate (vocal channel, both modes) ---
    layout.add (std::make_unique<P> (ParameterID { "gateThresh", 1 }, "Gate Threshold",
                                     NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -45.0f, A().withLabel ("dB")));
    layout.add (std::make_unique<P> (ParameterID { "gateAttack", 1 }, "Gate Attack",
                                     NormalisableRange<float> (0.1f, 100.0f, 0.01f, 0.4f), 5.0f, A().withLabel ("ms")));
    layout.add (std::make_unique<P> (ParameterID { "gateDecay", 1 }, "Gate Decay",
                                     NormalisableRange<float> (5.0f, 1000.0f, 0.1f, 0.4f), 150.0f, A().withLabel ("ms")));

    // --- "Band" mode: FFT bin mask ---
    layout.add (std::make_unique<P> (ParameterID { "binThresh", 1 }, "Bin Threshold",
                                     NormalisableRange<float> (-100.0f, -10.0f, 0.1f), -55.0f, A().withLabel ("dB")));
    layout.add (std::make_unique<P> (ParameterID { "binMaxCut", 1 }, "Bin Max Cut",     // mask gain range: -20 .. 0 dB
                                     NormalisableRange<float> (0.0f, 20.0f, 0.1f), 12.0f, A().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "binInvert", 1 }, "Bin Mask Invert", false));

    // --- "Spectral" mode: compressor on the filtered vocal, and the power threshold for ducking ---
    layout.add (std::make_unique<P> (ParameterID { "compThresh", 1 }, "Comp Threshold",
                                     NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -18.0f, A().withLabel ("dB")));
    layout.add (std::make_unique<P> (ParameterID { "compRatio", 1 }, "Comp Ratio",
                                     NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.5f), 3.0f, A().withLabel (":1")));
    layout.add (std::make_unique<P> (ParameterID { "compAttack", 1 }, "Comp Attack",
                                     NormalisableRange<float> (0.1f, 100.0f, 0.01f, 0.4f), 10.0f, A().withLabel ("ms")));
    layout.add (std::make_unique<P> (ParameterID { "compRelease", 1 }, "Comp Release",
                                     NormalisableRange<float> (10.0f, 1000.0f, 0.1f, 0.4f), 150.0f, A().withLabel ("ms")));
    layout.add (std::make_unique<P> (ParameterID { "bandThresh", 1 }, "Band Threshold",
                                     NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -40.0f, A().withLabel ("dB")));

    // --- Ducking ---
    layout.add (std::make_unique<P> (ParameterID { "duckAmount", 1 }, "Duck Amount",
                                     NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.7f));
    layout.add (std::make_unique<P> (ParameterID { "maxCut", 1 }, "Max Cut",
                                     NormalisableRange<float> (0.0f, 36.0f, 0.1f), 12.0f, A().withLabel ("dB")));

    // --- Output gain stages: vocal and instrument levels, applied before the sum ---
    layout.add (std::make_unique<P> (ParameterID { "vocalLevel", 1 }, "Vocal Gain",
                                     NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, A().withLabel ("dB")));
    layout.add (std::make_unique<P> (ParameterID { "instLevel", 1 }, "Instrument Gain",
                                     NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, A().withLabel ("dB")));

    return layout;
}

bool VocalMixerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != AudioChannelSet::stereo())
        return false;

    if (layouts.getChannelSet (true, 0) != AudioChannelSet::stereo())
        return false;

    // Both stereo inputs are required by design, but tolerate a host that leaves the vocal input disabled.
    const auto in2 = layouts.getChannelSet (true, 1);
    return in2 == AudioChannelSet::stereo() || in2.isDisabled();
}

void VocalMixerAudioProcessor::resetDspState()
{
    ducker.reset();
    compressor.reset();
    for (auto& f : vocalBp) f.reset();

    for (auto& d : vocalDelay) std::fill (d.begin(), d.end(), 0.0f);

    delayPos = 0;
    bandEnv = 0.0f;
    gateEnv = 0.0f;
    gateGain = 0.0f;
    gateOpen = false;
}

void VocalMixerAudioProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    currentSampleRate.store (newSampleRate);

    ducker.prepare (sampleRate);

    for (auto& d : vocalDelay) d.assign ((size_t) SpectralDucker::latencySamples, 0.0f);

    resetDspState();
    lastMode = (int) (pMode->load() + 0.5f);
    lastBpLo = lastBpHi = -1.0f;      // force the band mask to be rebuilt for the new sample rate

    vocalGainSmoothed.reset (sampleRate, 0.02);
    instGainSmoothed.reset (sampleRate, 0.02);
    vocalGainSmoothed.setCurrentAndTargetValue (Decibels::decibelsToGain (pVocalLevel->load(), -60.0f));
    instGainSmoothed.setCurrentAndTargetValue (Decibels::decibelsToGain (pInstLevel->load(), -60.0f));

    setLatencySamples (SpectralDucker::latencySamples);
}

void VocalMixerAudioProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    auto instIn  = getBusBuffer (buffer, true, 0);    // Stereo Input 1: instruments
    auto vocalIn = getBusBuffer (buffer, true, 1);    // Stereo Input 2: vocal
    auto output  = getBusBuffer (buffer, false, 0);

    if (instIn.getNumChannels() < 2 || output.getNumChannels() < 2)
        return;

    const bool haveVocal = vocalIn.getNumChannels() >= 2;

    const float* iL = instIn.getReadPointer (0);
    const float* iR = instIn.getReadPointer (1);
    const float* vL = haveVocal ? vocalIn.getReadPointer (0) : nullptr;
    const float* vR = haveVocal ? vocalIn.getReadPointer (1) : nullptr;
    float* oL = output.getWritePointer (0);
    float* oR = output.getWritePointer (1);

    // ---- Feed the spectrum display with the raw inputs (before the output overwrites them) ----
    {
        constexpr int chunk = 256;
        float v[chunk], s[chunk];

        for (int start = 0; start < numSamples; start += chunk)
        {
            const int n = std::min (chunk, numSamples - start);
            for (int i = 0; i < n; ++i)
            {
                v[i] = haveVocal ? 0.5f * (vL[start + i] + vR[start + i]) : 0.0f;
                s[i] = 0.5f * (iL[start + i] + iR[start + i]);
            }
            vocalScope.push (v, n);
            instScope.push (s, n);
        }
    }

    // ---- Mode: 0 = "Spectral" (band-power duck), 1 = "Band" (vocal bin mask) ----
    const int mode = (int) (pMode->load() + 0.5f);
    if (mode != lastMode)
    {
        resetDspState();
        lastMode = mode;
    }
    ducker.setMode (mode == 0 ? SpectralDucker::Mode::fixedBand : SpectralDucker::Mode::vocalMask);

    // ---- Band-pass filter (both modes) and its magnitude response per FFT bin ----
    {
        float lo = pBandLow->load();
        float hi = std::max (pBandHigh->load(), lo * 1.2f);
        hi = std::min (hi, 0.45f * (float) sampleRate);
        lo = std::min (lo, hi / 1.2f);
        bpCoeffs.set (sampleRate, lo, hi);

        if (lo != lastBpLo || hi != lastBpHi)
        {
            lastBpLo = lo;
            lastBpHi = hi;

            std::array<float, SpectralDucker::numBins> m {};
            for (int k = 0; k < SpectralDucker::numBins; ++k)
                m[(size_t) k] = bpCoeffs.magnitudeAt (MathConstants<double>::twoPi * (double) k / (double) SpectralDucker::fftSize);

            ducker.setBandMask (m.data());   // ducking is limited to the frequencies the filter passes
        }
    }

    // ---- Noise gate parameters ----
    const float thrLin   = Decibels::decibelsToGain (pGateThresh->load());
    const float closeLin = thrLin * 0.7f;   // ~3 dB hysteresis
    const float attackMs = std::max (0.05f, pGateAttack->load());
    const float decayMs  = std::max (0.05f, pGateDecay->load());
    const float atkCoef  = std::exp (-1.0f / (attackMs * 0.001f * (float) sampleRate));
    const float decCoef  = std::exp (-1.0f / (decayMs  * 0.001f * (float) sampleRate));
    const float detRel   = std::exp (-1.0f / (0.02f * (float) sampleRate));   // 20 ms detector release

    vocalGainSmoothed.setTargetValue (Decibels::decibelsToGain (pVocalLevel->load(), -60.0f));
    instGainSmoothed.setTargetValue (Decibels::decibelsToGain (pInstLevel->load(), -60.0f));

    const int delayLen = SpectralDucker::latencySamples;

    auto runGate = [&] (float vl, float vr, float& gl, float& gr) noexcept
    {
        const float level = std::max (std::abs (vl), std::abs (vr));
        gateEnv = level > gateEnv ? level : gateEnv * detRel;

        if (! gateOpen && gateEnv > thrLin)        gateOpen = true;
        else if (gateOpen && gateEnv < closeLin)   gateOpen = false;

        const float gTarget = gateOpen ? 1.0f : 0.0f;
        const float c = gTarget > gateGain ? atkCoef : decCoef;
        gateGain = gTarget + c * (gateGain - gTarget);

        gl = vl * gateGain;
        gr = vr * gateGain;
    };

    if (mode == 0)
    {
        //======================================================================
        // "SPECTRAL" MODE
        //   vocal -> noise gate -> band-pass -> compressor -> [measure band power] -> gain
        //   The measured power ducks ONLY the band-pass range of the instrument channel.
        //======================================================================
        compressor.setParams (sampleRate, pCompThresh->load(), pCompRatio->load(),
                              pCompAttack->load(), pCompRelease->load());

        const float bandThrDb = pBandThresh->load();
        const float duckAmt   = pDuckAmount->load();
        const float maxCutDb  = pMaxCut->load();
        const float pwrAtk    = std::exp (-1.0f / (0.005f * (float) sampleRate));   //   5 ms
        const float pwrRel    = std::exp (-1.0f / (0.120f * (float) sampleRate));   // 120 ms
        constexpr float rangeDb = 12.0f;   // band power this far above the threshold = full ducking

        float cutDb = 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float il = iL[i];                          // read before the output overwrites input 1
            const float ir = iR[i];
            const float vl = haveVocal ? vL[i] : 0.0f;
            const float vr = haveVocal ? vR[i] : 0.0f;

            float gl, gr;
            runGate (vl, vr, gl, gr);

            // Band-pass the gated vocal, then compress it
            const float bl = vocalBp[0].process (bpCoeffs, gl);
            const float br = vocalBp[1].process (bpCoeffs, gr);

            const float cg = compressor.computeGain (std::max (std::abs (bl), std::abs (br)));
            const float cl = bl * cg;
            const float cr = br * cg;

            // Measure the band's power AFTER the compressor (before the gain stage)
            const float m = 0.5f * (cl + cr);
            const float p = m * m;
            bandEnv = p + (p > bandEnv ? pwrAtk : pwrRel) * (bandEnv - p);

            const float levelDb = 10.0f * std::log10 (bandEnv + 1.0e-12f);
            const float depth = jlimit (0.0f, 1.0f, (levelDb - bandThrDb) / rangeDb);
            cutDb = duckAmt * maxCutDb * depth;
            ducker.setBandGain (std::exp (-cutDb * 0.11512925f));

            // Latency-matched vocal path
            const float dl = vocalDelay[0][(size_t) delayPos];
            const float dr = vocalDelay[1][(size_t) delayPos];
            vocalDelay[0][(size_t) delayPos] = cl;
            vocalDelay[1][(size_t) delayPos] = cr;
            if (++delayPos >= delayLen) delayPos = 0;

            // Instruments through the ducker (vocal input is not used in this mode)
            float xl, xr;
            ducker.setGate (gateGain);
            ducker.processSample (0.0f, il, ir, xl, xr);

            // Per-channel gain stages, then sum
            const float vg = vocalGainSmoothed.getNextValue();
            const float ig = instGainSmoothed.getNextValue();
            oL[i] = xl * ig + dl * vg;
            oR[i] = xr * ig + dr * vg;
        }

        duckMeterDb.store (cutDb);
    }
    else
    {
        //======================================================================
        // "BAND" MODE
        //   vocal -> band-pass -> noise gate -> FFT bin mask (optionally inverted)
        //   Bins used by the vocal duck the same bins of the instruments (within the band-pass range).
        //======================================================================
        ducker.setParameters (pBinThresh->load(), pDuckAmount->load(), pBinMaxCut->load(), pBinInvert->load() > 0.5f);

        for (int i = 0; i < numSamples; ++i)
        {
            const float il = iL[i];                          // read before the output overwrites input 1
            const float ir = iR[i];
            const float vl = haveVocal ? vL[i] : 0.0f;
            const float vr = haveVocal ? vR[i] : 0.0f;

            // Band-pass BEFORE the gate
            const float fl = vocalBp[0].process (bpCoeffs, vl);
            const float fr = vocalBp[1].process (bpCoeffs, vr);

            float gl, gr;
            runGate (fl, fr, gl, gr);

            // Latency-matched vocal path
            const float dl = vocalDelay[0][(size_t) delayPos];
            const float dr = vocalDelay[1][(size_t) delayPos];
            vocalDelay[0][(size_t) delayPos] = gl;
            vocalDelay[1][(size_t) delayPos] = gr;
            if (++delayPos >= delayLen) delayPos = 0;

            ducker.setGate (gateGain);
            float xl, xr;
            ducker.processSample (0.5f * (gl + gr), il, ir, xl, xr);

            const float vg = vocalGainSmoothed.getNextValue();
            const float ig = instGainSmoothed.getNextValue();
            oL[i] = xl * ig + dl * vg;
            oR[i] = xr * ig + dr * vg;
        }

        duckMeterDb.store (0.0f);
    }

    gateGainMeter.store (gateGain);
}

AudioProcessorEditor* VocalMixerAudioProcessor::createEditor()
{
    return new VocalMixerAudioProcessorEditor (*this);
}

void VocalMixerAudioProcessor::getStateInformation (MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VocalMixerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VocalMixerAudioProcessor();
}
