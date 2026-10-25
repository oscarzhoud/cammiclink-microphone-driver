# CamMicLink Microphone Driver

**A small, MIT-licensed virtual microphone driver for macOS.**

[English](README.md) | [简体中文](README.zh-CN.md)

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: macOS 12+](https://img.shields.io/badge/platform-macOS%2012%2B-lightgrey)
![Type: AudioServerPlugIn](https://img.shields.io/badge/type-AudioServerPlugIn-informational)

CamMicLink Microphone is an AudioServerPlugIn (a userspace HAL plug-in) that
gives every CoreAudio application a system-wide virtual microphone. No kernel
extension, no privileged daemon, no third-party runtime: one `.driver` bundle in
`/Library/Audio/Plug-Ins/HAL`.

This is the virtual microphone driver developed for the
[CamMicLink](https://oscarzhou.com/cammiclink/) app, extracted into a standalone
project and released under the MIT license.

## Why another virtual microphone driver?

On macOS, the open-source virtual audio driver I first considered using was
BlackHole, which is licensed under GPL-3.0. Copyleft is a feature for some
projects, but it does not work for a closed-source app or for App Store
distribution: bundling or linking GPL code would require releasing the whole app
under the GPL.

I needed a driver I could ship inside a proprietary commercial application, so I
wrote one myself — using Apple's official NullAudio sample for reference, with
help from GPT and DeepSeek — and released it under MIT. Anyone may use it,
modify it, embed it in a commercial product, or even sell it, with no obligation
to open their own code. That is my way of giving something back to the
open-source community that made this project possible.

The driver is an independent implementation. It contains no BlackHole code and
no code from any other third-party virtual audio driver. The CFPlugIn skeleton
follows Apple's MIT-licensed *NullAudio* sample (included unmodified in
`reference/`); the device topology, clock and audio engine are independent
rewrites.

## What it does

The driver exposes two linked devices:

```text
  Producer: phone bridge, network receiver, file player, DAW, ...
        │  writes PCM addressed by sample time
        ▼
  ┌─────────────────────────────────────────────────────┐
  │ CamMicLink Private Writer  (hidden, output only)    │
  │ UID com.oscarzhou.cammiclink.writer.v3              │
  └──────────────────────────┬──────────────────────────┘
                             │  timestamp-addressed PCM ring
                             ▼
  ┌─────────────────────────────────────────────────────┐
  │ CamMicLink Microphone  (public, input only)         │
  │ UID com.oscarzhou.cammiclink.microphone.v3          │
  └───────┬───────────────────────────┬─────────────────┘
          ▼                           ▼
   Zoom / WeChat / Teams       OBS / QuickTime / ...
```

- **CamMicLink Microphone** is what recording apps see. It has an input stream
  and *no* output stream, so a conferencing app cannot accidentally write to the
  microphone and overwrite the real producer.
- **CamMicLink Private Writer** is an output-only device with
  `kAudioDevicePropertyIsHidden = 1`. It does not appear in normal device
  enumeration and cannot be made a default output, but a producer can find it by
  UID and write into it.
- Both devices share one continuous 48 kHz sample clock. Audio is addressed by
  absolute sample timestamp, not by FIFO order, which means several clients can
  record the same virtual microphone at once without consuming each other's
  samples, and a client that joins late gets silence for the past instead of
  someone else's stale audio.

## Features

- **No kext, no helper daemon.** A single AudioServerPlugIn bundle, loaded by
  `coreaudiod`.
- **Input-only public device.** Recording clients get read access only.
- **Hidden writer.** Keeps the device list clean; never becomes a default output.
- **Timestamp addressing.** A 65,536-frame ring where every frame carries its
  absolute sample time. Unwritten, overwritten or out-of-range frames read as
  silence and old audio is never replayed.
- **Multiple readers.** Reads do not consume; concurrent clients can capture the
  same time range. Only one writer may be active at a time.
- **Real-time safe IO.** No allocation, no locks, no file IO and no logging in
  the IO path. Stereo frames are published with a single 64-bit lock-free atomic.
- **Continuous clock.** Join/leave of clients never resets the clock or clears
  the ring; a long idle gap is computed from real elapsed time.
- **Bounded work.** A circuit breaker keeps pathological callback storms from
  turning into unbounded work; degraded reads return deterministic silence.
- **Fixed, predictable format.** 48 kHz, stereo, interleaved Float32.
- **Tested.** Offline property/timeline regression plus a live HAL probe.
- **MIT licensed.** Use it in open-source or proprietary software.

## Devices and identifiers

| Object | UID | Streams | Visible to apps |
| --- | --- | --- | --- |
| CamMicLink Microphone | `com.oscarzhou.cammiclink.microphone.v3` | input only | yes, selectable / default input |
| CamMicLink Private Writer | `com.oscarzhou.cammiclink.writer.v3` | output only | hidden (`IsHidden=1`), reachable by UID |
| CamMicLink Box | `com.oscarzhou.cammiclink.box.v3` | — | container object only |

The plug-in bundle identifier is `com.oscarzhou.cammiclink.audiodriver`; the
installed bundle is `/Library/Audio/Plug-Ins/HAL/CamMicLinkMic.driver`.

The driver deliberately exposes no volume, mute, data-source, EQ, AGC or
denoising controls. It moves samples and nothing else.

## Requirements

- macOS 12 Monterey or later.
- Xcode Command Line Tools (`xcode-select --install`).
- Administrator rights for installation only.

The universal binary contains `arm64` and `x86_64` slices. It was built and
tested on macOS 27.0 / Apple Silicon; the Intel slice is built but has not been
run on Intel hardware.

## Build

```bash
./build.sh
```

This produces:

| Path | Contents |
| --- | --- |
| `build/CamMicLinkMic.driver` | the driver bundle |
| `build/hal_probe` | topology / tone / capture probe |
| `build/test_driver` | offline regression binary |
| `artifacts/CamMicLink-Microphone-0.3.0.pkg` | installer package |
| `artifacts/Uninstall-CamMicLink-Microphone.pkg` | uninstaller package |
| `artifacts/offline-test-results.txt` | last offline test log |

By default the driver is **ad-hoc signed**, which is enough for local use. For
distribution, sign with your own Developer ID:

```bash
CML_SIGN_ID="Developer ID Application: Your Name (TEAMID)" \
CML_INSTALLER_ID="Developer ID Installer: Your Name (TEAMID)" \
./build.sh
```

No certificate or team identifier is hard-coded in the repository.

## Install and uninstall

```bash
sudo ./scripts/install.sh     # copy the built driver and restart coreaudiod
sudo ./scripts/uninstall.sh   # remove only CamMicLinkMic.driver
```

`install.sh` replaces only `CamMicLinkMic.driver`; other audio plug-ins are
never touched. Restarting `coreaudiod` interrupts audio for a moment.

Alternatively, install `artifacts/CamMicLink-Microphone-0.3.0.pkg`. Packages are
unsigned unless you set `CML_INSTALLER_ID`, so Gatekeeper will warn on other
Macs.

## Quick start

After installing, verify the topology and run a loopback test. Stop any app
that is already feeding the private writer first; only one writer can run at a
time.

```bash
./build/hal_probe inspect          # must print "Topology: PASS"

# terminal 1: feed a 440 Hz tone into the hidden writer
./build/hal_probe tone 15 512

# terminal 2: record from the public microphone
./build/hal_probe record 10 512
```

`record` fails if the captured tone is silent, clipped, discontinuous or not
close to 440 Hz. It reads only the virtual microphone UID, never the physical
microphone.

Then open any audio or video app, choose **CamMicLink Microphone** as its input,
and feed audio from your own writer. A full multi-process test (IOProc, AUHAL and
AudioQueue at 8–48 kHz) is available as:

```bash
python3 tools/run_live_tests.py --seconds 8
```

## Feeding audio from your own app

Find the hidden writer by UID and start an output IOProc on it:

```c
CFStringRef uid = CFSTR("com.oscarzhou.cammiclink.writer.v3");
AudioObjectPropertyAddress a = {
    kAudioHardwarePropertyTranslateUIDToDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};
AudioDeviceID writer = kAudioObjectUnknown;
UInt32 size = sizeof(writer);
AudioObjectGetPropertyData(kAudioObjectSystemObject, &a,
                           sizeof(uid), &uid, &size, &writer);
```

Create the IOProc with `AudioDeviceCreateIOProcID(writer, callback, context,
&proc)`, start it with `AudioDeviceStart`, and in the callback fill
`output->mBuffers[0]` with interleaved stereo Float32 frames. Use
`outputTime->mSampleTime` as the sample position: the timestamp is what places
your audio on the shared clock, so do not keep a private frame counter instead.

Only the first writer is accepted. A second `AudioDeviceStart` on the writer
fails with `kAudioHardwareIllegalOperationError` until the first one stops.

For a complete, dependency-free implementation, see the `callback()` and `run()`
functions in [`tools/hal_probe.c`](tools/hal_probe.c).

Recording needs no special code: select the public device by UID, or just pick
"CamMicLink Microphone" in the app's normal input settings.

## Format and timing contract

| Property | Value |
| --- | --- |
| Sample rate | 48,000 Hz (fixed; the driver rejects 44.1 kHz and other rates) |
| Channels | 2, interleaved |
| Sample format | Float32, packed, native endian |
| Reported input latency | 2,048 frames ≈ 42.67 ms |
| Safety offset | 0 |
| Zero-timestamp period | 512 frames |
| Ring buffer | 65,536 frames ≈ 1.37 s |
| Max IO callback | 8,192 frames |

The driver performs no sample-rate conversion and no DSP. Clients that need
another rate convert on their side (AudioQueue does; AUHAL requires the device
rate — see [Apple TN2091](https://developer.apple.com/library/archive/technotes/tn2091/_index.html)).
The 42.67 ms figure is the HAL input latency reported
by the device, not the end-to-end latency of a call: the producer, network and
client buffers add to it.

Both devices use the same clock, anchored once when the plug-in is initialized.
Writing is addressed by `mOutputTime.mSampleTime`, reading by
`mInputTime.mSampleTime`. HAL applies the latency compensation to input
timestamps; the driver does not subtract it a second time.

## Testing

The offline suite compiles the real driver sources with AddressSanitizer and
UndefinedBehaviorSanitizer and checks:

- exact property sizes and payload boundaries for every supported selector;
- input/output direction and rejection of unsupported formats and sample rates;
- sample-exact continuity across 11 writer/reader block-size combinations,
  including two readers with different block sizes joining at different times;
- digital silence, refusal of a second writer, reader start/stop isolation,
  silence after writer stop, ring overwrite and invalid timestamps;
- clock continuity across a ten-minute gap.

[`docs/TEST-RESULTS.md`](docs/TEST-RESULTS.md) records the live measurements:
eight concurrent clients, a 440 Hz tone recovered at 439.87–440.05 Hz from
8 kHz to 48 kHz, with zero time breaks, invalid timestamps, bad buffers or
overflow.

Not covered by the automated tests: real phone-to-Mac calls in WeChat, Telegram
or Tencent Meeting, the DSP those apps apply after capture, runs longer than
30 minutes, Intel hardware, and every macOS release. Synthetic-tone correctness
is not a guarantee about how a third-party app will sound.

## Limitations

- macOS only. This is a CoreAudio HAL plug-in, not a cross-platform audio API.
- One writer, no mixing. Mix multiple producers in your app before writing.
- Fixed 48 kHz stereo Float32. No per-device format negotiation.
- The writer is hidden and cannot be selected in Audio MIDI Setup; address it by
  UID from your producer.
- Not a general-purpose audio router: no per-app capture, no multi-device
  routing, no effects.
- Ad-hoc-signed local builds are for development. For distribution, sign with a
  Developer ID and notarize the package.
- Version 0.3.0. The v3 UIDs and bundle identifiers are treated as stable API.

## Project layout

```text
src/
  CamMicLinkDriver.c     AudioServerPlugIn interface, properties, IO entry points
  CMLAudioCore.c/.h      shared clock, timestamp-addressed ring, atomics
tests/
  test_driver.c          offline contract and timeline regression
tools/
  hal_probe.c            live topology, tone writer, capture and SRC probes
  run_live_tests.py      multi-process live test runner
reference/
  NullAudio.c            Apple's unmodified sample
  LICENSE.txt            Apple's MIT license
  UPSTREAM.md            download URL and archive SHA-256
scripts/
  install.sh             install the built driver
  uninstall.sh           remove it
docs/
  TEST-RESULTS.md        live test record and caveats
build.sh                 universal build, ad-hoc/Developer ID signing, packages, tests
```

## License

MIT — see [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

`reference/NullAudio.c` is Copyright © 2024 Apple Inc., also MIT-licensed. The
driver follows that sample's CFPlugIn interface and property conventions; its
implementation is independent. No GPL code, including BlackHole, is included.

Apple, macOS, CoreAudio and AudioServerPlugIn are trademarks of Apple Inc. This
project is not affiliated with or endorsed by Apple.

## Contributing

Issues and pull requests are welcome. Please keep the IO path free of
allocation, locks, file IO and logging, keep the device format fixed at 48 kHz
stereo Float32, and run `./build.sh` before opening a pull request. For changes
to the ring or clock logic, include a regression case in `tests/test_driver.c`.
