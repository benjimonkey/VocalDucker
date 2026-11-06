# VocalMixer

VST3 plugin (JUCE). Stereo Input 1 = instruments, Stereo Input 2 = vocal, Stereo Output.
The instrument channel is ducked according to the vocal. Vocal Gain and Inst Gain scale each channel before the sum.
Latency: 512 samples (constant, reported to the host). The plugin starts in SPECTRAL mode.

Mode switch (top left): SPECTRAL | BAND. The GUI is laid out as the signal flow.

SPECTRAL: vocal -> noise gate -> band-pass -> compressor -> [band power measured here] -> gain
          The band power ducks only the frequencies passed by the band-pass filter (instruments are never filtered).
BAND:     vocal -> band-pass -> noise gate -> 512-pt FFT bin mask (INVERT button available)
          Bins used by the vocal duck the same bins of the instruments, within the band-pass range.
          Bin mask gain range is 0 to -20 dB. A BIN GAINS display shows the gain of every bin.

In both modes the amber trace on the spectrum display shows how each frequency is being ducked.

Build: GitHub Actions (.github/workflows/main.yml) builds Windows and macOS VST3s; download from the run's Artifacts.
Local: cmake -B build && cmake --build build --config Release
