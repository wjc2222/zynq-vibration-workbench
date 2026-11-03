# Board console bridge

`vibration_serial_bridge` is a Linux QtCore event loop with nonblocking stdin and stdout. It launches exactly one existing `vibration_capture_service` executable through the unchanged `CaptureClient`/`CaptureTransport`. The unchanged `LivePreviewModel` validates the raw preview again and computes spectra from the original contiguous raw records. QtWidgets is a link dependency because the model shares the existing `PlotSeries` type; no window or display server is started.

Build independently of the workbench:

```sh
cmake -S software/serial_bridge -B /tmp/serial-bridge -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/serial-bridge -j4
```

An optional project integration is `add_subdirectory(software/serial_bridge)` after the existing core subdirectory. The root CMake file is deliberately unchanged. The ARM toolchain is supplied normally with `-DCMAKE_TOOLCHAIN_FILE=...`. `build_and_verify.sh` runs reproducible host fixture QA, cross-compiles the ARM binary, and retains logs, source hashes, ELF metadata and test evidence in `artifacts/serial-pc-v1/bridge-baud-v2/`. The earlier115200-only binary and its source snapshot remain in `artifacts/serial-pc-v1/bridge/`.

Production launch:

```sh
vibration_serial_bridge --service /usr/bin/vibration_capture_service
```

`--baud 115200|460800|921600` declares the actual rate selected by the caller. The default remains115200. It selects a1000ms preview timer at115200 and a250ms timer at460800/921600; `hello` returns the selected `baud` and `preview_interval_ms`. The bridge does not alter terminal speed. The PC must synchronize its port and `stty` before launching it. Higher rates require physical serial verification before use.

`--test-fixture` and `--hardware-self-test` are mutually exclusive explicit QA modes. There is no automatic fallback. The caller owns serial-port setup and terminal-mode restoration. The bridge never opens a COM device or a JTAG connection. Its stdin/stdout must already be a raw console stream. Every outgoing line follows [contract-v1.json](contract-v1.json); stdout contains only packets and diagnostics go to stderr.

The PC sends a `ping` every three seconds. Fifteen seconds without a valid, uniquely identified request cancels acquisition and exits. EOF, shutdown, output disconnection and reliable-queue overflow also close the owned child. Input lines are bounded to8192 bytes, nesting to16, and request IDs to65536 per session. Duplicate object keys, duplicate IDs, unknown fields and arbitrary paths are rejected. Output is nonblocking with a1MiB/64-packet reliable queue and one replaceable preview. Repeated same-state progress and preview error counters are coalesced to1Hz; state changes and terminal messages are reliable. The current partially transmitted packet finishes intact; a terminal event discards every queued preview.

Each bridge launch creates a private0700 UUID directory below `/tmp/vibration-serial-records`. This is volatile storage. A bounded recovery scan validates complete `.BIN`/`.meta.json` pairs from earlier launches in the same RAM boot; it does not delete captures. The registry admits at most64 complete records and new capture admission is bounded to256MiB total plus free-space reserve. Core CRC/index/timestamp integrity, source declarations, header CRC identity and acquisition-plan metadata must agree before records become visible. Owned ordinary files are opened read-only with `O_NOFOLLOW`; inode/device/size/mtime/ctime identities and both SHA256 hashes bind downloads. If either file changes, all chunks are rejected until successful recovery. Record IDs are registry lookups, never a general file API. Downloads require idle acquisition/analysis, `download_begin`, and chunks of at most3072 bytes with CRC32.

Live output is a latest-only1Hz display summary at115200 and4Hz at460800/921600. Waveform first/min/max/last reduction preserves gap markers and at most156 points per channel. Spectrum bins are min/max peak preserving, with at most128 displayed bins per channel and the model's own spectrum time range. The preview packet targets at most6KiB compressed. Its diagnosis is always `preview_only`/`not_classified`; physical vibration identity, calibration and fault classification are not inferred.

On successful save, asynchronous full-record analysis loads and validates the complete saved BIN through the shared core. It retains the default65536-point FFT, full-record time statistics, overlapping spectrum averaging, order bands and the core's conservative diagnosis. Only the transmitted spectrum display arrays are reduced after `to_json` to at most256 bins, with `display_decimated` and `original_bin_count`; actual FFT, bin width, segments and all computed statistics remain intact. The waveform point budget is320. The result explicitly states `scope: complete_record`. Preview and complete-result ranges are separate. Source metadata and both source-file hashes accompany the result.

Host tests use synthetic service fixtures over actual pipes. They verify packet decompression/hash/sequence, input bounds, duplicate keys/IDs, arbitrary-path rejection, source flags, default FFT/statistics equivalence to the independently invoked core CLI, display peak preservation, original metadata download, chunk CRC/SHA256, pair mutation, recovery, backpressure/cancel, cancellation before start acknowledgement, EOF, bad-preview CRC, production no-fallback and heartbeat loss. Host tests and cross-compilation do not establish physical serial, board ADC, mechanical classification, HDMI or touch acceptance.
