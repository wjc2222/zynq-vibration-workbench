# Continuous capture service

`vibration_stream_service` is additive. It does not alter the finite capture
service or its CAPDDR2 files. Production requires the streaming ABI in
`hardware/combined/include/ax7z020_capture_stream.h` and the existing
`/dev/ax7z020-capture` device. `--test-fixture` explicitly selects synthetic host
data; it is never enabled implicitly when a device is missing.
`--hardware-test` explicitly requests the real PL test pattern through the
production ring driver and is mutually exclusive with `--test-fixture`. It sets
`AX7Z020_STREAM_START_TEST_PATTERN`; every chunk's driver status must confirm
that source. It is still synthetic data, labelled `hardware_test`, and follows
the same real DMA/SD path as ADC acquisition.

The JSONL schema is `vibration.stream.v1`. Input lines are limited to 8192 bytes.
Every request requires `schema`, `request_id` (1..128 characters), and `command`.
`id` is accepted as an alias and both fields are returned; conflicting aliases,
duplicate JSON keys, unknown fields and unknown commands are rejected.

```json
{"schema":"vibration.stream.v1","request_id":"start-1","command":"start","plan":{"mode":"timed","sample_rate_hz":50000,"duration_seconds":3600,"pulses_per_rev":1,"output_directory":"/mnt/captures/vibration","segment_seconds":10}}
{"schema":"vibration.stream.v1","request_id":"stop-1","command":"stop"}
```

`mode` is `timed` or `continuous`. Continuous plans must omit
`duration_seconds`; timed durations are integer 1..86400 seconds. Rates are
50000, 25000, 10000 or 5000 Hz. PPR is integer 1..4096. Segment duration is
integer 1..40 seconds and defaults to 10. Session totals use 64-bit arithmetic
and are encoded as decimal strings (50000 × 86400 = 4320000000 records).
All global indices, ticks, frame/segment indices and cumulative counters use
decimal strings, preserving values beyond JSON's exact integer range.

Commands are `capabilities`, `start`, `stop`, `status`, `shutdown`.
Responses have `type: response`, `ok`, `request_id`, `id`, `source`, and
`emitter: stream_service`. Capabilities are under `capabilities`, including
`available`, `reason`, and `backend`. A start/status/stop response has `job`.
Errors include `error_code` and `error_message`.

Events have `type: event`, `event`, the originating start request's `id` and
`request_id`, `job_id`, `session_id`, `source`, `synthetic`,
`hardware_confirmed`, and `emitter`. `source` is `test_fixture`, `hardware_test`
or `adc_unconfirmed`; a real driver does not prove sensor wiring or calibration.

* `job_status`: `state`, `plan`, `total_records`, `records_received` (alias
  `records_captured`), `durable_records` (alias `records_persisted`),
  `segment_count`, `elapsed_seconds`, `directory`, and error fields.
* `preview`: one original 40 ms chunk, at most 2000 records; `frame_seq`,
  `global_first_index`, `first_tick`, `last_tick`, `record_count`,
  `raw_base64`, `crc32`, `record_bytes: 32`, `sample_rate_hz`, `clock_hz`,
  `ppr`, `adc_range_uv`, `channel_map`. The CRC is IEEE reflected CRC32 over
  the exact raw bytes. Raw record indices are never rebased.
* `segment_ready`: `segment_id`, `segment_index`, `global_first_index`,
  `record_count` (alias `count`), ticks, `path`, `meta_path`, payload/header
  CRCs, format and source metadata. Paths are absolute.
* `session_done`: status fields plus `session_complete` (all received chunks
  were durably committed) and `target_complete` (the timed total was reached).
  Terminal states are `completed` (timed total), `stopped` (normal manual stop
  or an early stop), and `failed`. An early timed stop can have
  `session_complete: true` and `target_complete: false`.

`stop`, `shutdown` and stdin EOF all request a boundary stop, drain committed
chunks and save the final partial segment before the worker exits. A storage
or integrity failure preserves previously published segments and reports
`failed`; it never describes the whole session as complete. `records_received`
counts validated chunks already read from the driver, even if a following SD
write fails. Driver `committed_records` only means validated DMA, not disk
durability.

Storage writes run independently of the stdout writer. Only one latest preview
is retained; slow consumers may lose UI frames while raw storage continues.
Control events have a bounded 256-message FIFO and are never replaced. If a
slot is unavailable for 100 ms, the transport fails with
`TRANSPORT_BACKPRESSURE`. The raw worker requests a boundary stop, drains and
publishes its tail instead of waiting behind stdout. The stdin loop detects
transport failure even when no further command or EOF arrives. Shutdown allows
up to two seconds to drain stdout, then exits with code 2 on an output failure.
Failed reliable-control delivery is explicitly reported on stderr and in an
atomic, exclusive `<session_id>.transport-error.json` notice on the selected
SD directory; the notice records the actual durable prefix and never claims a
successful complete session. If SD cannot store that notice, stderr reports
that additional failure and existing segments remain untouched. A normal raw
data result never serves as proof that the controller received its terminal
event. The driver has a bounded 256 × 40 ms ring and reports overruns as failures.

## CAPSTRM1 segment layout

All integers and the unchanged 32-byte raw records are little endian. The file
is a 4096-byte header followed by `completed_records × 32` bytes. The maximum
file size remains 64 MiB. A segment is independently complete, although its
session may later fail or stop early. CAPDDR2 remains supported by the core
loader with its original version and CRC rules.

| Offset | Type | Meaning |
|---:|---|---|
| 0 | 8 bytes | ASCII `CAPSTRM1` |
| 8 | u32 | Format version 1 |
| 12,16,20 | u32 | Header bytes 4096, record bytes 32, segment READY state 2 |
| 24,28 | u32 | Sample rate, clock 50000000 Hz |
| 32,36,40 | u32 | Segment requested/completed records (equal), payload bytes |
| 44 | u32 | Error 0 (published complete segment) |
| 48,52 | u32 | Streaming STR_STATE and DMA status snapshots |
| 56,60 | u32 | Dropped records 0, PPR |
| 64,72 | u64 | Segment first/last tick |
| 80,84 | u32/i32 | Channel map 0x50301, ADC range 5000000 µV |
| 88,92 | u32 | Speed filter 5 and timeout 100000000 cycles |
| 96,100 | u32 | Payload CRC32, header CRC32 |
| 104 | u64 | Session-global first record index |
| 112 | 16 bytes | Nonzero session UUID, RFC 4122/network byte order |
| 128 | u64 | Zero-based segment index |
| 136 | u64 | Session total records; 0 means continuous |
| 144 | u32 | Full segment target records (tail may contain fewer) |
| 148 | u32 | Format flags; bit 0 synthetic, other bits reserved |
| 152 | u32 | Header CRC coverage bytes, exactly 4096 |
| 156..4095 | bytes | Reserved, zero |

For the header CRC, clear bytes 100..103 and compute CRC32 across all 4096
bytes. This covers the global index, session ID, segment index, total, target
and source flags. CAPSTRM1 `hw_status` uses streaming STR_STATE; legacy
CAPDDR2's hardware TEST bit 6 does not apply to the new format. Synthetic
status in CAPSTRM1 is determined from the CRC-protected format flag and
metadata. Index/tick continuity is verified across every chunk and across
segment boundaries; the core verifies each segment independently.

## SD publication and failure behavior

The output directory must already exist, be writable, be absolute, and have no
symbolic link components. The service never mounts, remounts, formats, or
deletes user data. Each segment checks available space for a full segment plus
metadata and a reserve (64 MiB by default; CLI `--reserve-bytes=N`).

A UUID-named private staging directory is created exclusively in the selected
directory. `data.BIN` is streamed directly to disk without holding a session or
segment in RAM. The final header and `data.meta.json` are fsynced, followed by
the staging directory. A Linux `renameat2(RENAME_NOREPLACE)` publishes the
directory, making the pair appear atomically, then the parent is fsynced before
`segment_ready` is emitted. Unsupported publication primitives fail closed.
Only this writer's unpublished temporary files are removed on failure.

If the parent-directory fsync fails after rename, the published pair is retained
for inspection, the session fails, and that segment is not added to the claimed
durable prefix. Power loss or SIGKILL can leave an unpublished `.partial`
directory; it is never represented as a complete segment or automatically
deleted during a later session.

## Host checks

```sh
cmake -S . -B /tmp/continuous-v4-stream-host -DVIBRATION_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build /tmp/continuous-v4-stream-host --target vibration_stream_service vibration_analyze
ctest --test-dir /tmp/continuous-v4-stream-host -R 'stream_service_storage_protocol|core_' --output-on-failure
```

Explicit fixture-only options are `--fixture-chunk-ms=N` (0..1000, default 40)
and `--fixture-fault=low_space|short_write|storage_after_first|device_gap`.
`short_write` performs a real 17-byte partial write followed by a synthetic
ENOSPC failure. Tests use temporary host directories and do not establish real
SD throughput, physical ADC continuity, UART/HDMI/touch behavior, or calibration.
