#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

VocalMixerAudioProcessor::VocalMixerAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Vocal",       AudioChannelSet::stereo(), true)   // Stereo Input 1
                          .withInput  ("Instruments", AudioChannelSet::stereo(), true)   // Stereo Input 2
                          .withOutput ("Output",      AudioChannelSet::stereo(), true)), // Stereo Output 1
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    pMode        = apvts.getRawParameterValue ("mode");
    pGateThresh  = apvts.getRawParameterValue ("gateThresh");
    pGateAttack  = apvts.getRawParameterValue ("gateAttack");
    pGateDecay   = apvts.getRawParameterValue ("gateDecay");
    pBinThresh   = apvts.getRawParameterValue ("binThresh");
    pDuckAmount  = apvts.getRawParameterValue ("duckAmount");
    pMaxCut      = apvts.getRawParameterValue ("maxCut");
    pVocalLevel  = apvts.getRawParameterValue ("vocalLevel");
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

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode",
                                                        StringArray { "Spectral", "Band" }, 0));

    // --- Noise gate (vocal channel, both modes) ---
    layout.add (std::make_unique<P> (ParameterID { "gateThresh", 1 }, "Gate Threshold",
                                     NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -45.0f, A().withLabel ("dB")));
    layout.add (std::make_unique<P> (ParameterID { "gateAttack", 1 }, "Gate Attack",
                                     NormalisableRange<float> (0.1f, 100.0f, 0.01f, 0.4f), 5.0f, A().withLabel ("ms")));
    layout.add (std::make_unique<P> (ParameterID { "gateDecay", 1 }, "Gate Decay",
                                     NormalisableRange<float> (5.0f, 1000.0f, 0.1f, 0.4f), 150.0f, A().withLabel ("ms")));

    // --- Spectral mode ---
    layout.add (std::make_unique<P> (ParameterID { "binThresh", 1 }, "Bin Threshold",
                                     NormalisableRange<float> (-100.0f, -10.0f, 0.1f), -55.0f, A().withLabel ("dB")));

    // --- Band mode: band-pass filter ---
    layout.add (std::make_unique<P> (ParameterID { "bandLow", 1 }, "BPF Low Cut",
                                     NormalisableRange<float> (20.0f, 8000.0f, 1.0f, 0.35f), 150.0f, A().withLabel ("Hz")));
    layout.add (std::make_unique<P> (ParameterID { "bandHigh", 1 }, "BPF High Cut",
                                     NormalisableRange<float> (200.0f, 20000.0f, 1.0f, 0.35f), 6000.0f, A().withLabel ("Hz")));
    layout.add (std::make_unique<P> (ParameterID { "bandThresh", 1 }, "Band Threshold",
                                     NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -40.0f, A().withLabel ("dB")));

    // --- Band mode: compressor on the filtered vocal ---
    layout.add (std::make_unique<P> (ParameterID { "compThresh", 1 }, "Comp Threshold",
                                     NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -18.0f, A().withLabel ("dB")));
    layout.add (std::make_unique<P> (ParameterID { "compRatio", 1 }, "Comp Ratio",
                                     NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.5f), 3.0f, A().withLabel (":1")));
    layout.add (std::make_unique<P> (ParameterID { "compAttack", 1 }, "Comp Attack",
                                     NormalisableRange<float> (0.1f, 100.0f, 0.01f, 0.4f), 10.0f, A().withLabel ("ms")));
    layout.add (std::make_unique<P> (ParameterID { "compRelease", 1 }, "Comp Release",
                                     NormalisableRange<float> (10.0f, 1000.0f, 0.1f, 0.4f), 150.0f, A().withLabel ("ms")));

    // --- Ducking (both modes) ---
    layout.add (std::make_unique<P> (ParameterID { "duckAmount", 1 }, "Duck Amount",
                                     NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.7f));
    layout.add (std::make_unique<P> (ParameterID { "maxCut", 1 }, "Max Cut",
                                     NormalisableRange<float> (0.0f, 36.0f, 0.1f), 12.0f, A().withLabel ("dB")));

    // --- Gain stage on the vocal path (both modes, after the compressor in Band mode) ---
    layout.add (std::make_unique<P> (ParameterID { "vocalLevel", 1 }, "Vocal Gain",
                                     NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f, A().withLabel ("dB")));

    return layout;
}

bool VocalMixerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != AudioChannelSet::stereo())
        return false;

    if (layouts.getChannelSet (true, 0) != AudioChannelSet::stereo())
        return false;

    // Second stereo input is required by design, but tolerate a host that leaves it disabled.
    const auto in2 = layouts.getChannelSet (true, 1);
    return in2 == AudioChannelSet::stereo() || in2.isDisabled();
}

void VocalMixerAudioProcessor::resetDspState()
{
    ducker.reset();
    compressor.reset();
    for (auto& f : vocalBp) f.reset();
    for (auto& f : instBp)  f.reset();

    for (auto& d : vocalDelay)   std::fill (d.begin(), d.end(), 0.0f);
    for (auto& d : bandOutDelay) std::fill (d.begin(), d.end(), 0.0f);

    delayPos = 0;
    outDelayPos = 0;
    bandEnv = 0.0f;
    gateEnv = 0.0f;
    gateGain = 0.0f;
    gateOpen = false;
}

void VocalMixerAudioProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    currentSampleRate.store (newSampleRate);

    for (auto& d : vocalDelay)   d.assign ((size_t) SpectralDucker::latencySamples, 0.0f);
    for (auto& d : bandOutDelay) d.assign ((size_t) SpectralDucker::latencySamples, 0.0f);

    resetDspState();
    lastMode = (int) (pMode->load() + 0.5f);

    vocalGainSmoothed.reset (sampleRate, 0.02);
    vocalGainSmoothed.setCurrentAndTargetValue (Decibels::decibelsToGain (pVocalLevel->load(), -60.0f));

    setLatencySamples (SpectralDucker::latencySamples);
}

void VocalMixerAudioProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    auto vocalIn = getBusBuffer (buffer, true, 0);
    auto instIn  = getBusBuffer (buffer, true, 1);
    auto output  = getBusBuffer (buffer, false, 0);

    if (vocalIn.getNumChannels() < 2 || output.getNumChannels() < 2)
        return;

    const bool haveInst = instIn.getNumChannels() >= 2;

    const float* vL = vocalIn.getReadPointer (0);
    const float* vR = vocalIn.getReadPointer (1);
    const float* iL = haveInst ? instIn.getReadPointer (0) : nullptr;
    const float* iR = haveInst ? instIn.getReadPointer (1) : nullptr;
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
                v[i] = 0.5f * (vL[start + i] + vR[start + i]);
                s[i] = haveInst ? 0.5f * (iL[start + i] + iR[start + i]) : 0.0f;
            }
            vocalScope.push (v, n);
            instScope.push (s, n);
        }
    }

    // ---- Mode ----
    const int mode = (int) (pMode->load() + 0.5f);   // 0 = Spectral, 1 = Band
    if (mode != lastMode)
    {
        resetDspState();
        lastMode = mode;
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
        // SPECTRAL MODE: FFT bins used by the vocal duck the same bins of the instruments
        //======================================================================
        ducker.setParameters (pBinThresh->load(), pDuckAmount->load(), pMaxCut->load());

        for (int i = 0; i < numSamples; ++i)
        {
            const float il = haveInst ? iL[i] : 0.0f;
            const float ir = haveInst ? iR[i] : 0.0f;

            float gl, gr;
            runGate (vL[i], vR[i], gl, gr);

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
            oL[i] = xl + dl * vg;
            oR[i] = xr + dr * vg;
        }

        duckMeterDb.store (0.0f);
    }
    else
    {
        //======================================================================
        // BAND MODE:
        //   vocal -> gate -> band-pass -> [measure power] -> compressor -> gain
        //   the measured band power ducks the same band of the instruments
        //======================================================================
        float lo = pBandLow->load();
        float hi = std::max (pBandHigh->load(), lo * 1.2f);
        hi = std::min (hi, 0.45f * (float) sampleRate);
        lo = std::min (lo, hi / 1.2f);
        bpCoeffs.set (sampleRate, lo, hi);

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
            const float il = haveInst ? iL[i] : 0.0f;
            const float ir = haveInst ? iR[i] : 0.0f;

            float gl, gr;
            runGate (vL[i], vR[i], gl, gr);

            // Band-pass the gated vocal
            const float bl = vocalBp[0].process (bpCoeffs, gl);
            const float br = vocalBp[1].process (bpCoeffs, gr);

            // Measure the band's output power (before compression)
            const float m = 0.5f * (bl + br);
            const float p = m * m;
            bandEnv = p + (p > bandEnv ? pwrAtk : pwrRel) * (bandEnv - p);

            const float levelDb = 10.0f * std::log10 (bandEnv + 1.0e-12f);
            const float depth = jlimit (0.0f, 1.0f, (levelDb - bandThrDb) / rangeDb);
            cutDb = duckAmt * maxCutDb * depth;
            const float g = std::exp (-cutDb * 0.11512925f);

            // Duck the same band of the instruments: remove (1 - g) of the band-passed copy.
            // At g = 1 this is an exact pass-through.
            const float ibl = instBp[0].process (bpCoeffs, il);
            const float ibr = instBp[1].process (bpCoeffs, ir);
            const float xl = il - (1.0f - g) * ibl;
            const float xr = ir - (1.0f - g) * ibr;

            // Compressor on the filtered vocal, then the gain stage
            const float cg = compressor.computeGain (std::max (std::abs (bl), std::abs (br)));
            const float vg = vocalGainSmoothed.getNextValue();

            const float sumL = xl + bl * cg * vg;
            const float sumR = xr + br * cg * vg;

            // Same latency as Spectral mode so the host's delay compensation never changes
            oL[i] = bandOutDelay[0][(size_t) outDelayPos];
            oR[i] = bandOutDelay[1][(size_t) outDelayPos];
            bandOutDelay[0][(size_t) outDelayPos] = sumL;
            bandOutDelay[1][(size_t) outDelayPos] = sumR;
            if (++outDelayPos >= delayLen) outDelayPos = 0;
        }

        duckMeterDb.store (cutDb);
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
