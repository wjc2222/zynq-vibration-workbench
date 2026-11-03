# Native Linux capture service

`vibration_capture_service` is a Qt Core/C++17 child process. The touch GUI
communicates with it through stdin/stdout JSONL described in
[`contract-v2.json`](contract-v2.json). It does not use a shell or the old PC
binary protocol. Standard output is reserved for protocol messages.

The production backend opens `/dev/ax7z020-capture` exclusively and verifies the
shared ABI, hardware identity and fixed capability values through ioctls.
A missing device, permission failure or capability mismatch stays unavailable.
The service never falls back to a simulated device. The local UAPI copy must
remain byte identical to `hardware/combined/include/ax7z020_capture.h`; this copy
lets the existing application source archive build without the hardware tree.

Live preview is specified in [`preview-contract-v1.json`](preview-contract-v1.json).
Each job must explicitly opt in with top-level `preview:true` on `start`. Missing
or false emits no preview events, preserving strict existing v2 clients. This
field is not part of the plan. The new CapturePage explicitly opts in.
It requires actual SG/committed-prefix features and a successful preview INFO
ioctl. The service copies only validated completed DMA descriptor data, in
chunks of at most 256 records, aggregates at most 4096 original 32-byte records
per frame and checks each slice CRC, index, timestamp and source before emitting
it. Preview lines have a 192 KiB limit; control messages retain 8192 bytes. Both
UAPI headers in `include/vibration/` must match the shared hardware originals.

A dedicated nonblocking stdout writer uses a bounded control FIFO and one
latest-preview slot. Slow readers lose preview frames with explicit gap counts;
they do not block capture or lose saved raw records. Terminal events discard
unsent previews. A client verifies its original start plan, the returned plan
and every preview's rate/PPR/count, as well as raw CRC and immutable identity.
Bad display frames are rejected independently; source or generation changes
quarantine preview for that job. An active connection loss reports unknown
device state and cannot claim the driver stopped. Complete SG captures also
compare the full payload CRC with the driver's final committed-prefix CRC.
Client and live-model payload decoding share a strict single-pass UTF16 base64
decoder: exact expected length, standard ASCII alphabet, padding placement and
zero pad bits are checked without a Latin1 copy or re-encoding. Each consumer
still verifies frame CRC and all record/source/identity checks independently.
Hardware preview polling follows a steady-clock 40 ms grid; expired slots are
skipped without catch-up bursts, and work time does not add another full period.
An explicit `AX7_CAPTURE_WIDE_PIPE=1` experiment requests a fixed 256 KiB Linux
stdout pipe before writing. The default keeps its original capacity. A regular
file or failed resize keeps the original behavior. Profiling records requested,
original and actual capacity, errno, partial writes and input chunk sizes.
This bounded buffer can reduce fragmented-frame wakeups; it cannot guarantee
uninterrupted preview during an arbitrarily stalled GUI. No global pipe limits,
permissions, JSON protocol, CRC/source checks or queue bounds are changed.

Each finite job polls the driver, supports cancellation, reads only a completed
payload, and checks actual counts, rate, source flags, record indices, timestamps
and ADC/SPEED validity. It writes the vendor-compatible CAPDDR2 layout with
header and payload CRC32 and runs the existing `vibration::load_capture` before
publication. PPR is an explicitly confirmed analysis value recorded in the
header and metadata; it is not an ADC hardware setting.

The output directory must already exist, be writable and have no symlink path
components. Staging files use exclusive creation and unpredictable names. The
metadata is flushed and published first, followed by a directory flush; the
unique BIN is then published without replacement and the directory flushed
again. Cancellation and commit are serialized. Failure removes only files
created by that job. No SD mounting, remounting, partitioning or formatting is
performed. A real ADC record remains source unconfirmed and uncalibrated.
Actual hardware TEST state forces synthetic metadata and cannot be overridden
through a GUI request.

For host QA only, explicitly launch `--test-fixture`. Its files always carry
synthetic metadata and `hardware_confirmed=false`. The optional
`--fixture-fault=driver|index|tick|flags|short|crc|publication|preview_crc|preview_unavailable` arguments exercise
device, integrity and publication failure paths. They require `--test-fixture`.
Production GUI startup must not pass these arguments.
The fixture emits synthetic progress at 40 ms intervals over the requested
finite duration. `preview_crc` corrupts only the emitted display CRC; the final
file still undergoes independent validation. `preview_unavailable` explicitly
emulates a backend without preview and does not substitute frames.

`--hardware-self-test` is a separate explicit board verification mode. It sends
the real driver's `START_TEST_PATTERN` flag, requires driver status and hardware
status bit 6 to agree with that request, and forces synthetic metadata. It never
substitutes the host fixture when a device is missing. It is mutually exclusive
with `--test-fixture`, and normal GUI startup must not pass it. Merely selecting
this mode does not establish the actual source: capabilities distinguish
`requested_source` from `actual_source`. The current acquisition board is wired
but the sensor is not connected; these modes do not establish mechanical-data
acceptance.

Host verification:

```sh
python3 -B software/acquisition/tests/test_service.py \
  --service BUILD/software/acquisition/vibration_capture_service \
  --analyze BUILD/software/core/vibration_analyze \
  --report BUILD/capture-service-host-tests.json
```

CTest registers this suite for non-cross Linux builds with Qt Core and Python.
The suite uses temporary directories and explicit synthetic fixtures. It does
not validate the physical ADC, SPEED input, Linux driver, SD persistence,
independent board startup or touch operation. Those remain board acceptance.
