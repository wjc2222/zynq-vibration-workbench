# SG DMA committed preview candidate

The current simple-DMA platform has one AXIS packet per complete capture, with
TLAST only on the final beat. Replacing its full S2MM_LENGTH with a20–40ms buffer
does not create a safe completion boundary. AMD PG021 requires the direct-mode
buffer to hold the largest incoming packet; a smaller buffer is undefined.
The length register is updated with actual received bytes at completion, so it
is not an in-flight DDR-commit counter. See the current official
[S2MM_LENGTH definition](https://docs.amd.com/r/en-US/pg021_axi_dma/S2MM_LENGTH-S2MM-DMA-Buffer-Length-Register-Offset-58h)
and [direct-mode sequence](https://docs.amd.com/r/en-US/pg021_axi_dma/Direct-Register-Mode-Simple-DMA).

The retained capture stream FIFO contains256 records (8192 payload bytes), plus
its separate output/pending state. At50kHz its conservative FIFO-only coverage
is5.12ms, not40ms. A software rearm design would depend on a finite interrupt
latency and would need different packet/FIFO design. Produced/sent counters
only describe PL activity; they do not prove memory writes are committed.

The new candidate instead requires AXI DMA SG with its M_AXI_SG descriptor
read/write path connected to the same HP1 DDR domain as S2MM data. Capture RTL,
record format and the single final TLAST remain unchanged. The entire finite
descriptor chain is posted before producer START. Each descriptor covers
rate/25 records:2000/1000/400/200 records at50/25/10/5kHz, respectively, giving
40ms blocks and32-byte-aligned payload regions. Last-block length is exact.
Descriptors are64-byte aligned, addresses are32-bit, SG length width is26,
cyclic mode and status/control stream are disabled. TAILDESC names the last BD;
its next pointer is valid but no BD is reused in this finite run.

Completion authority is the contiguous BD.Completed prefix. After observing
Completed, the driver executes dma_rmb, checks exact transferred bytes,
first-only RXSOF/final-only RXEOF, original record indices, exact tick spacing,
validity/speed flag consistency and actual source/rate. Only then does it
advance committed_records and its IEEE CRC32. See AMD's
[S2MM descriptor status definition](https://docs.amd.com/r/en-US/pg021_axi_dma/S2MM_STATUS-S2MM-Status).
The final IOC remains packet/EOF based; the driver polls completed BDs every5ms
without depending on per-BD interrupts or PS rearm. All overflow/ADC/DMA/SG,
byte/count/source/rate errors invalidate the job; no guessed prefix is published.

SG start order follows PG021's explicit S2MM SG sequence:CURDESC, RS=1,
wait for HALTED to deassert, then TAILDESC. It also matches the pinned Linux6.6.70
xilinx_dma_start_transfer/xilinx_dma_start implementation. See the official
[SG startup sequence](https://docs.amd.com/r/en-US/pg021_axi_dma/Scatter/Gather-Mode).
At final IOC, CAP/DMA registers and BD ownership are resampled to avoid combining
a pre-EOF CAP snapshot with a new interrupt. The latched IOC survives W1C ack.
There is a bounded100ms convergence window for final visibility/state; every
original exact final predicate remains required. Invalid Completed descriptors
and actual hardware errors fail immediately. The window never permits reading
or publishing an incomplete BD.

The separate ax7z020_capture_preview.h preserves the base ABI. GET_PREVIEW_INFO
returns64 bytes with generation, verified prefix/counts/CRC, source/rate and
state. COPY_PREVIEW returns at most256 original32-byte records in a fixed8272
byte structure, with a slice CRC and validity flags. The80-byte header fits
the ioctl encoding together with its8192-byte payload. Requested records and
reserved fields are checked, unused bytes are zero, stale generation is ESTALE,
uncommitted data is EAGAIN, and failed/cancelled/idle reads are refused. No user
physical address, pointer, mmap or raw-memory device is provided. The large ioctl
result is heap allocated to avoid the ARM kernel stack limit.

Read-only preview during RUNNING is provisional for display, not final dataset
acceptance. JOB_COMPLETE is set only after all BDs/bytes, total captured/sent,
no drops/errors and final DMA idle+IOC agree. The existing ordinary read still
returns the whole payload only after READY. Service finalization can compare
its payload CRC against GET_PREVIEW_INFO committed_crc32 while it still owns the
fd; closing it releases the job's payload and descriptors. Preview frames may
be skipped by the GUI, while all raw records remain in the finite DMA buffer.

Cancel/close/remove abort the producer and quiesce/reset both DMA and SG before
freeing either allocation. IRQ is synchronized and pending work is drained on
remove. Reset failure poisons reuse and quarantines both allocations/device
reference until reboot. A different compatible string and DMASR.SGIncluded check
prevent this driver from operating the existing simple-DMA image. Old simple
sources and build artifacts remain unchanged.

The new build is output-combined-sg-preview-r1 and requires the matched new
SG-enabled XSA/bit/ps7_init. With128MiB CMA and1GiB DDR, the absolute driver limit
uses67104768 payload bytes, at worst671104 descriptor bytes at5kHz, and7372800
display bytes, leaving59069056 CMA bytes before other users. Service40s limits
use at most1000 descriptors (64000 bytes) at any supported rate. Allocation
success and latency remain runtime checks, never assumed from this budget.

Initial goals are25Hz committed-preview availability and independent50fps UI
drawing. These are targets until measured on the board, not a60fps promise.
Required new acceptance includes SG-only probe/refusal on simple hardware,
all rates, single/partial/final BD cases, preview stable bytes/CRC/generation,
full-data CRC agreement, late error/cancel rejection, rapid cancel/restart,
maximum-buffer completion and simultaneous display/SD read-only checks. Real
sensors, physical touch/HDMI and mechanical classification remain separate.
