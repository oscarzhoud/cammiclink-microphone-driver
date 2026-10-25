# Apple reference

Source: https://developer.apple.com/documentation/coreaudio/creating-an-audio-server-driver-plug-in

Archive: https://docs-assets.developer.apple.com/published/430ad6501f6f/CreatingAnAudioServerDriverPlugIn.zip

Downloaded 2026-10-24. SHA-256: `860334dfa9e83f2afb749a890ca5a79fc7d3d572d4e116ff9242bec458be4cb0`.

`NullAudio.c` and `LICENSE.txt` are unmodified files from that archive. The new driver follows the sample's CFPlugIn interface and object/property conventions; its device topology and IO implementation are rewritten. Apple's original `DoIOOperation` treats `ioMainBuffer` as raw stream-format PCM (`memset(..., frames * 8)`), not AudioBufferList.
