# VocalMixer

VST3 plugin (JUCE). Stereo Input 1 = instruments, Stereo Input 2 = vocal, Stereo Output.
The instrument channel is ducked according to the vocal. Vocal Gain and Inst Gain scale each channel before the sum.
The plugin starts in Spectral mode.
Latency: 512 samples in both modes (constant, reported to the host).

Mode switch (top left): SPECTRAL | BAND. The GUI is laid out as the signal flow.

Spectral:  vocal -> band-pass -> noise gate -> 512-pt FFT bin mask
           bins used by the vocal duck the same bins of the instruments.
Band:      vocal -> noise gate -> band-pass -> compressor -> [band power measured here] -> gain
           band power ducks the whole instrument channel (the instruments are not filtered).

Build: GitHub Actions (.github/workflows/main.yml) builds Windows and macOS VST3s; download from the run's Artifacts.
Local: cmake -B build && cmake --build build --config Release
