# SpeexDSP 1.2.1 preprocessor subset

The `upstream/` sources and license are taken from the Xiph.Org release.
`speexdsp_config_types.h` is the local generated-header equivalent.
The sole upstream C patch compiles out the optional residual-echo hook,
keeping the same noise-only code path as a null echo state.

- Archive: https://downloads.xiph.org/releases/speex/speexdsp-1.2.1.tar.gz
- SHA-256: `8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d`
- Build: fixed-point C with the upstream KISS FFT. No echo canceller or codec.
- Local wrapper: bounded caller-owned workspace, allocation failure unwind,
  single-threaded construction, and fresh per-talk history from an arena
  snapshot. KISS FFT symbols are prefixed so they cannot overlap Opus.
