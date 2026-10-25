# Test results — 0.3.0

Recorded 2026-10-24 (UTC+8) on macOS 27.0, Xcode 27.1, Apple Silicon. This page
summarises the validation run for the initial open-source release. The live
test command is shown below; raw logs were not committed.

## Installation and topology

- Installed to `/Library/Audio/Plug-Ins/HAL/CamMicLinkMic.driver`, version
  `0.3.0` (build `300`).
- Developer ID Application signature verified; universal `arm64` + `x86_64`
  binary. Only the Apple Silicon slice was executed.
- The installed executable matched the build output:
  `2555f759f76a569902dce531eba00d0a7c6b4de7130646e1556d26d832107a82`.
- `hal_probe inspect` passed: the public device has one input stream and no
  output stream; the writer is `hidden=1`, has one output stream and no input
  stream, and does not appear in normal device enumeration.

## Offline regression

Built the real driver sources with AddressSanitizer and UndefinedBehaviorSanitizer.

- 98 property-size and payload-boundary checks, with guard bytes after every
  returned buffer.
- Direction checks: public input only, hidden writer output only. Unsupported
  sample rates and formats are rejected; the device rate is fixed at 48 kHz.
- 11 writer/reader block-size configurations matched sample by sample, including
  1024/512, 1024/256, 1024/480, 1024/441, 512/1024, 64/2048, and two readers with
  different block sizes joining at different times.
- Digital silence, rejection of a second writer, reader start/stop isolation,
  silence after the writer stops, stale ring timestamps and invalid timestamps
  all passed.
- The shared clock advanced correctly across a ten-minute gap.
- ASan/UBSan reported no errors.

Offline tests validate the algorithm against the driver entry points, not the
real HAL. The live suite below covers the HAL side.

## Live test

```bash
python3 tools/run_live_tests.py --seconds 8
```

One process fed a 440 Hz, amplitude 0.3 sine wave into the hidden writer,
generated directly from the device sample time in the IOProc — no producer
thread, network or application FIFO was involved. Eight recording processes ran
in parallel. Every process exited 0; every reader reported `timeBreaks=0`,
`invalidTimes=0`, `badBuffers=0`, `overflow=0` and `abnormalJumps=0`.

Results after ignoring the first 0.5 s:

| Reader | Client rate | Measured tone | Max adjacent jump | Abnormal jumps |
| --- | ---: | ---: | ---: | ---: |
| IOProc, 512-frame request | 48000 | 439.8987 Hz | 0.017276 | 0 |
| IOProc, 256-frame request | 48000 | 440.0320 Hz | 0.017276 | 0 |
| AUHAL, mono | 48000 | 439.8987 Hz | 0.017276 | 0 |
| AudioQueue, mono | 8000 | 439.8667 Hz | 0.101597 | 0 |
| AudioQueue, mono | 16000 | 440.0540 Hz | 0.051771 | 0 |
| AudioQueue, mono | 32000 | 439.9509 Hz | 0.025910 | 0 |
| AudioQueue, mono | 44100 | 439.9504 Hz | 0.018805 | 0 |
| AudioQueue, mono | 48000 | 439.9307 Hz | 0.017276 | 0 |

Notes:

- The local HAL used **192-frame** hardware callbacks for both IOProcs, not the
  requested 512 or 256 frames. The requested size is a preference, not a
  promise; the actual sizes were reported by the probe. Other block sizes are
  covered by the offline regression.
- AudioQueue returned roughly 20 ms blocks: 160 frames at 8 kHz, 320 at 16 kHz,
  640 at 32 kHz, 882 at 44.1 kHz and 960 at 48 kHz, with a tail block on stop.
- At low sample rates a correct sine has larger sample-to-sample jumps, so the
  discontinuity threshold is computed per rate instead of using the 48 kHz
  figure. The near-zero-sample ratio is not treated as a drop-out rate; the
  probe also checks frequency, RMS, peak and timestamp continuity.
- `coreaudiod` used about 4–5% CPU during the run. This was a short observation,
  not a long-term stability guarantee.

## Tooling issues found during the run

1. One early multi-process launch timed out and produced no valid audio result;
   that run was not counted as a success.
2. AUHAL initially returned `-10863` when asked to read at a rate different from
   the device. Per Apple TN2091 the AUHAL client rate must match the device, so
   AUHAL remained a 48 kHz test and other rates went through AudioQueue. The
   driver was not changed to work around this.
3. AudioQueue can deliver empty buffers while stopping. The probe now ignores
   stop notifications without PCM and stops re-enqueueing during shutdown; the
   audio pass thresholds were not relaxed.

Early failed logs were kept during development; the final results above are from
the committed source version and its exit codes.

## Not covered

- Real end-to-end calls from an iPhone through WeChat, Telegram or Tencent
  Meeting.
- The denoising, echo cancellation, codecs and network buffering those apps apply
  after capture.
- Runs longer than 30 minutes, Intel hardware, and every macOS release.
- App-level listening or monitoring paths; these belong to the application, not
  the driver.

A synthetic tone passing through the virtual driver does not prove that a
third-party app's processed call audio will sound correct. If audio is distorted
in a real call, capture the driver output and the remote recording separately
before adding gain or filtering inside the driver.
