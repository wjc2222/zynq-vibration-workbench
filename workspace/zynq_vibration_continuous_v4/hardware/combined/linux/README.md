# MRG1 Linux capture driver

`ax7z020_capture.c` is built into the new combined Linux kernel. The driver owns
CAP2 control `0x43c10000/64KiB`, simple AXI DMA S2MM `0x40400000/64KiB` and
IRQ_F2P[1] (GIC62, DT SPI30) as one platform device. No second DMAengine, UIO or
GPIO binding may claim those resources. Display keeps its existing separate
MMIO, interrupt, clocks and reset handling.

The clock is the actual board U18 50MHz input distributed by the combined RTL,
represented as a fixed clock in `capture.dtsi`. It is not PS FCLK1. Register
revision `MRG1`, clock, record size, ADC range, channel map, speed filter,
speed timeout and feature/rate masks are checked at probe. No device node or
hardware-confirmed capabilities exist when probe fails.

The fixed UAPI is `../include/ax7z020_capture.h`. All ioctl structures contain
fixed-width fields and zero reserved fields. Open is exclusive (node mode0600).
`GET_CAPS` follows successful hardware probe; `START` accepts only 5/10/25/50kHz,
1..2097024 records, and the explicit optional test-pattern flag. It invalidates
the old result and resets this fd's read offset. Source mode and actual rate
are read back; test source is returned through status flags.

For every start the producer is aborted and DMA reset must complete before a
new allocation/reuse. The Linux DMA API allocates a 32-bit coherent buffer, up
to67104768 bytes; no user physical address, mmap or `/dev/mem` is used. Configure,
ADC reset/readiness, DMA destination, DMA LENGTH, then producer START are ordered
in that sequence. LENGTH is the last DMA arm operation. Both IRQ completion and
a bounded monitor check the DMA/error/status/count contract. READY requires
DMA idle+IOC, exact byte length, captured=sent=requested, no drops/errors, and
producer source_done+stream_done without active.

Read blocks while capturing (`O_NONBLOCK` returns EAGAIN), returns payload only
after READY, supports sequential partial reads, then EOF. `pread`, seek and mmap
are unsupported. Failed/idle/cancelled reads return the corresponding errno.
Status `error_code` is a positive Linux errno. CANCEL is idempotent when inactive.
Close cancels active work, quiesces DMA and releases the completed buffer; the
next open starts IDLE. Remove marks retained fds unavailable and synchronizes
the IRQ/work before freeing driver state. If DMA reset/quiescence fails, the
driver refuses reuse and deliberately quarantines the coherent memory/device
reference until reboot, rather than freeing a possible active DMA destination.

`combined_kernel.fragment` raises the common CMA budget from64MiB to128MiB.
The maximum capture plus the existing double framebuffer needs74477568 bytes,
leaving59740160 bytes of CMA margin. The DT retains1GiB DDR. CMA has no forced
physical start; Linux memblock excludes loaded kernel/initrd/DT reservations.
The old standalone `0x10000000/0x10001000` capture addresses are never reused.

Build and DT/config verification are performed by
`linux/combined/scripts/build-kernel.sh` into the independent WSL
`~/ax7z020-build/output-combined-mrg1`. This copies the kernel once and reuses
the old toolchain read-only; it does not rebuild Qt or mutate the display output.
The derived kernel is not proof of ADC/DMA/display/real-touch runtime acceptance.
