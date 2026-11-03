# Persistent stream UART bridge

`vibration_stream_bridge` is a separate Linux Qt Core executable. It runs
`vibration_stream_service` as its acquisition owner; the finite
`software/serial_bridge` is unchanged. It never acquires raw records itself,
mounts/formats media or substitutes RAM storage. Only explicitly selected
completed records can be deleted under the guarded `delete_records` contract.

Production launch:

```sh
/usr/bin/vibration_stream_bridge --baud 115200 \
  --service /usr/bin/vibration_stream_service \
  --output-root /mnt/capture-data/vibration
```

The default root is `/mnt/capture-data/vibration`. It must already exist.
Production capabilities and every start require an actual ready, writable,
non-root mount whose device is `/dev/mmcblkNpN`. A missing mount, a read-only
partition, a symlink component or an inaccessible directory fails closed.
`hello.data.servicecaps` and `service_ready.data.servicecaps` preserve service
`available` and add `storage: sd`, `persistent`, `storage_available`,
`storage_reason`, device/filesystem/mount, output directory and `volatile: false`.

`--test-fixture` explicitly enables host synthetic data and host temporary
directories. `--fixture-chunk-ms=N`, `--fixture-fault=NAME` require that mode.
`--hardware-test` explicitly requests the driver's PL test pattern, uses the
real SD mount, and is mutually exclusive with host fixtures. Its source is
`hardware_test`, synthetic true and hardware confirmed true; this is still
test data. Normal production source remains `adc_unconfirmed`. The shared
`--reserve-bytes=N` option is forwarded to the service.

## UART contract

Input JSONL uses the existing `vibration.serial.v1` schema and request IDs
matching `[A-Za-z0-9_-]{1,64}`. Lines are limited to 8192 bytes. Duplicate keys,
unknown fields, repeated IDs in the bounded last-4096 replay window and invalid
plans fail with structured errors. Output is the unchanged monotonically
sequenced `qcompress-base64` envelope, SHA256 over the exact decoded compact
JSON, max wire line 131072 and decoded JSON 2097152 bytes. The existing Python
`PacketDecoder` and `RecordDownload` are used in the integration tests.

Responses retain `{id,command,ok,data}` or `{id,command,ok:false,error:{code,message}}`.
Events retain `{event,data}`. Service requests use a separate internal
`vibration.stream.v1` ID namespace and exact response correlation.

* `hello`, `ping`: capabilities/readiness and bounded-queue/drop counters.
* `start`: `plan` has `mode: timed|continuous`, `sample_rate_hz` in
  5000/10000/25000/50000, `pulses_per_rev` 1..4096. Timed duration is an integer
  1..86400 seconds. Continuous mode must omit `duration_seconds`.
  `segment_seconds` is optional, defaults to 10, range 1..40. An optional
  `output_directory` must exactly equal the configured root.
  Response data includes flattened `job_id`, plan/status and the original `job`.
* `stop`: optional current `job_id`; `accepted:true` means boundary stop was
  requested. Completion is established by the subsequent terminal event.
* `list_records`: `cursor` defaults to `""`, `limit` defaults to 100, range
  1..100. Returns `records`, `next_cursor`, `has_more`, rejected count and
  `integrity: header_and_metadata_only`. Cursor is an opaque listed segment ID.
  Each call retains at most limit+1 sorted candidate names and reads only the
  selected page's 4096-byte headers and bounded metadata. No startup scan,
  persistent in-memory registry or whole-card payload hashing is performed.
  New UUID sessions can sort before a previous cursor; refresh page one to see
  additions. Rejected candidates still advance the cursor.
* `analyze_record`: opaque `record_id`. Full BIN/metadata SHA256, core payload
  CRC/record integrity and file-identity checks precede full core analysis.
  Response scope is `complete_record` with `segment_only:true`. It describes
  the selected saved segment and does not certify the whole session.
* `download_begin`: opaque `record_id`, full integrity/identity checks and
  SHA256 manifest for `data.BIN` plus `data.meta.json`, max chunk 3072 bytes.
* `download_chunk`: same record ID, `kind: bin|meta`, integer `offset`, `length`
  1..3072. Bounds, both file identities, base64 and CRC32 are checked. Files
  stay open through the download; replacement, truncation and modification fail.
  Receiving both complete files in contiguous offset order automatically releases
  the active download reference.
* `download_end`: no arguments, or optional matching `record_id`. Releases the
  current connection's download reference when cancelled or locally rejected.
* `delete_records`: explicit `record_ids` array, 1..100 unique bounded strings.
  The caller must first show the exact selection/count/size and obtain a separate
  confirmation. There is no whole-card, wildcard, path or automatic deletion.
  Capture/start/Stop-drain or a missing terminal `session_done`, any file analysis,
  waiting user file task, active download or another deletion rejects the batch
  with `BUSY`; deletion is never queued behind another task.

  Production holds `/dev/ax7z020-capture` open exclusively through the entire
  transaction, including fsync. The driver's global exclusive-open rule rejects
  concurrent HDMI/other-process acquisition; it also prevents capture from
  starting until deletion releases its descriptor. Missing/busy/unverified
  devices reject the operation. Production never proceeds without this lease.
  Explicit host fixtures simulate that lease with `flock` on
  `<root>/.vibration-fixture-device.lock`, or the fixture-only
  `--fixture-device-lock ABSFILE`; that option is rejected outside fixture mode.

  Each selected ID is checked independently. Only directly managed CAPSTRM1
  `UUID-segment` directories whose header/metadata bind to the same session,
  source and path, and which contain exactly ordinary `data.BIN` and
  `data.meta.json`, are eligible. Legacy IDs, traversal, symlinks, unknown files,
  missing pairs and changed identities are preserved with per-item errors.
  A verified directory is atomically moved to a unique private `.deleting-*`
  name; the moved directory inode is checked again before known-file unlink.
  Only the two named files and that empty verified directory can be removed.
  No recursive unlink is used. If an entry was replaced during the move, both
  original file descriptors and the unknown replacement are preserved; an
  unknown replacement is never blindly restored over an entry.

  Response data has `results`, `requested_records`, `deleted_records` and
  `deleted_bytes`. Each result has `record_id`, `deleted`, actual removed `bytes`,
  `files_removed`, `directory_removed`, `durability_confirmed`, and an optional
  `{code,message}` error. `deleted:true` requires both files, the directory and
  parent fsync to succeed. Partial failures report actual bytes and any
  `remaining_staging_id` for explicit inspection; no later automatic cleanup
  broadens the deletion. Successfully removed data cannot be undone here.
* `shutdown`: acknowledge `accepted:true,durability_confirmed:true` only after
  service shutdown has drained the driver, published/fsynced the tail, emitted
  its terminal status and exited successfully. There is no two-second kill.
  A known failed session still permits an accepted graceful shutdown, with
  `durability_confirmed:false` and its explicit failed durable-prefix status.
  A missing terminal session event instead returns a shutdown error.
  On a real tty, `tcdrain` completes the final UART transmit queue before the
  bridge exits and lets the parent shell restore its baud rate.

`job_status`, `segment_ready` and `session_done` preserve flat service fields
inside the UART event's `data`. All global indices, ticks and cumulative raw
counts stay decimal strings. Timed 50000 Hz × 86400 seconds is `"4320000000"`.
Only `completed`, `stopped` and `failed` terminate capture. Segment publication
and per-segment analysis never terminate the session.

## UI work and backpressure

Every incoming preview is checked for source tuple, canonical base64, CRC,
index/tick continuity, format and flags. Even the first preview is bound to
the accepted start plan and the explicit launch backend's capabilities.
Status/events are bound to the current session, plan and exact 64-bit total;
event counters cannot decrease or claim a durable prefix beyond received raw.
A missing preview chunk clears the
display window; the bridge does not invent continuity. A contiguous original
raw window of at most 0.25 seconds / 32768 samples provides per-axis
mean/min/max/peak-to-peak/RMS/AC-RMS in volts. Statistics use every raw sample
in that window. Speed preserves explicit null gaps. Preview diagnosis remains
`preview_only/not_classified`; sensor calibration and real faults are unconfirmed.

V5 preview display fields are additive to the serial envelope and do not change
the acquisition service, SD format, sample rate or raw persistence:

* `waveform_detail`: `schema: vibration.waveform.raw.v1`,
  `encoding: s16le-interleaved-base64`, `sample_count` (normally 512),
  `first_index` and `time_origin_index` decimal strings, `sample_rate_hz`,
  `adc_range_uv`, `raw_base64`. Bytes are original adjacent CH1/CH2/CH3 int16
  little endian samples. Time is `(first_index - time_origin_index + i)/Fs`;
  volts are `int16 * adc_range_uv / 1e6 / 32768`. Live selects the newest
  512 samples, so at 50 kHz it shows 10.24 ms rather than pretending that
  512 points describe the whole 0.25-second overview. Live origin is zero.
* `waveform_envelope`: `schema: vibration.waveform.envelope.v1`, `unit: V`,
  `start_s`, `end_s`, three `channels` of `[start_s,end_s,min,max]` intervals,
  `representation: min_max_intervals`, `connect_buckets:false`. There are
  32 buckets at 115200 or 64 at higher baud. Each bucket covers every raw
  sample in its inclusive bounds. Render independent range bars; never join
  buckets as a waveform. This is an overview, not decimated raw sine samples.
* `spectrum`: `available`, `start_s`, `end_s`, `max_hz:2000`, `unit:V`,
  three `channels`. Each channel has `encoding:f32le-base64`,
  `peak_amplitude_base64`, `bin_count`, `first_hz:0`, `bin_width_hz`,
  `fft_size`, `segment_count`, `window:hann`, `window_duration_s`, `enbw_hz`
  and `display_decimated:false`. Every valid 0–2 kHz bin is transmitted,
  with frequency `i * bin_width_hz`; values are IEEE little endian float32.
  FFT target is 8192 with actual power-of-two size bounded by the contiguous
  0.25-second input. At 5/10/25/50 kHz this is 1024/2048/4096/8192, with at
  most 410 bins per axis. FFT window averaging uses the full raw window.

The old `voltage` field is empty and `voltage_representation` explicitly says
`use_waveform_detail`. A legacy client must not connect sparse extrema as if
they were adjacent ADC samples. No interpolation or smoothing is introduced.
Detail, overview and raw statistics have distinct time extents; a UI must show
which one is being viewed. PC and HDMI use the same sample/time semantics,
although HDMI's local raw ring does not need the UART transport.

Summary work is off the UART event loop and generated every 2 seconds at
115200 baud, or 4 Hz at 460800/921600. The writer enforces an 80% UART byte
budget. One 115200 preview is capped at 18432 wire bytes (two seconds of that
budget). If needed, the optional overview is omitted with
`overview_omitted_due_to_budget:true`; detail and complete spectrum are not
silently thinned. An over-budget final preview is counted and dropped.
The deterministic three-axis noise fixture measures the real compressed
wire size; a sine is not a safe worst-case compression assumption.
There is one replaceable summary and one replaceable background-analysis
result. Full saved-segment analysis uses one worker and retains only the
latest waiting segment. Its event scope is `completed_segment`; it must not
replace live plots or clear the active capture state.

Automatic segment diagnostics carry features only, without waveform, spectral
arrays or speed trace. Explicit `analyze_record` keeps the original complete
core statistics/diagnosis and labelled legacy waveform/spectrum summaries,
and adds `result.display_detail`, `result.display_envelope` (128 buckets),
and `result.display_spectrum` using the same schemas above. Detail starts at
the first 512 selected-record samples; `time_origin_index` is that record's
first global index, giving record-local seconds. The separate display FFT
targets 8192 and averages the entire selected record; original 65536-target
diagnostic analysis remains unchanged. Display spectral amplitudes are volts,
recovered with the absolute configured gain/sensitivity factor from the
nominal g analysis. This does not assert physical sensor calibration.
Its explicit response may take several seconds at 115200; control has priority
at each JSONL packet boundary and cannot interrupt an already started packet.
The remote overview is not raw zoom; download the paired raw record for that.

`ping` reports decimal-string `preview_dropped_records` (service preview
gaps), `preview_dropped_frames` (replaced UART summaries),
`preview_rejected_frames`, and `analysis_skipped_segments` (replaced waiting
analysis inputs or outputs). These are UI losses, never claims about raw loss.
Raw losses/errors are the service's explicit failed session and durable prefix.

Reliable control output is bounded to 64 messages / 1 MiB. Queue exhaustion,
UART EOF, SIGINT/SIGTERM and 15-second heartbeat loss request graceful service
shutdown and wait for tail publication. UART output loss discards display/control
delivery while the service still seals the tail. No worker forcibly kills the
raw writer. One user list/analyze/download-begin request can wait while the
single segment-file worker is occupied. It runs before the next background
analysis and retains its original ID, without an early success response.
Only another request exceeding that one waiting slot returns `BUSY`.
Raw acquisition continues separately.

Board stdin/stdout/stderr can share the physical UART. Bridge diagnostics,
forwarded child-service stderr and Qt runtime messages therefore go to syslog
when stderr is a tty, without console/stderr echo flags. On a non-tty pipe they
remain on stderr. Only framed packets may enter the protocol tty; diagnostic
text must never be interleaved with JSONL.

Directory access opens every component with Linux `openat`/`O_NOFOLLOW`, and
selected reads retain directory/file descriptors and compare device, inode,
length, modification and change times before/after reads. Record IDs cannot
escape the configured root. CAPSTRM1 synthetic status comes from offset 148
bit 0 plus matching metadata; legacy TEST bit 6 is not used.

## Verification

The top-level project registers this directory on Linux. It can also be built
standalone with `cmake -S software/stream_bridge -B /tmp/stream-bridge-host`.

```sh
cmake -S . -B /tmp/continuous-v4-bridge-host -DVIBRATION_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build /tmp/continuous-v4-bridge-host --target vibration_stream_bridge vibration_stream_service
ctest --test-dir /tmp/continuous-v4-bridge-host -R stream_bridge_protocol --output-on-failure
```

Twenty-four real bridge-to-service host integration cases cover timed raw/CRC and
pagination, full selected download SHA256/identity mutation, real raw-window
statistics/FFT/speed, segment analysis while active, manual continuous stop,
shutdown-after-tail, EOF including startup EOF, 64-bit totals, failed storage
with a preserved prefix, slow UART consumption with raw continuity, and unsafe
IDs/symlinks/duplicate keys. Three cases relay the real fixture service through
a test-only fault proxy to reject mismatched first-preview parameters/source,
a foreign terminal job and a missing terminal session event. A separate case
checks bounded user-file request queuing and original response correlation.
Seven deletion cases cover explicit selection and untouched originals, active
capture/analysis/download/unsealed-session rejection, download release, per-item
unsafe/legacy/symlink/unknown-file refusal, batch bounds, external simulated
device lease ownership/release and the fixture-only guard. A separate C++
regression replaces a directory between validation and rename and verifies
that neither the selected original files nor unknown replacement files are
deleted. Its injection hook exists only in the test target's compilation.
Pipe and shared-tty cases also inject real child stderr diagnostics; the tty
case verifies strict packet decoding, final tail/ACK ordering and the tty drain
exit path, without using a physical board or claiming real UART timing proof.
Tests create only
explicit host fixture data under
temporary directories and remove it afterwards. Host and ARM compilation do
not establish physical ADC continuity, real SD throughput, UART hardware,
calibration, or mechanical fault accuracy.
