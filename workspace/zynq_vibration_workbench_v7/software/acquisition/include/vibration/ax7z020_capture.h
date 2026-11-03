/* SPDX-License-Identifier: MIT
 * Fixed-width Linux userspace ABI for the combined capture/display platform.
 * No userspace pointers, physical addresses or native-size fields cross it.
 */
#ifndef AX7Z020_CAPTURE_UAPI_H
#define AX7Z020_CAPTURE_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define AX7Z020_CAPTURE_ABI 1u
#define AX7Z020_CAPTURE_HW_ID 0x32415043u
#define AX7Z020_CAPTURE_REVISION 0x4d524731u /* MRG1 */
#define AX7Z020_CAPTURE_MAX_RECORDS 2097024u
#define AX7Z020_CAPTURE_RECORD_BYTES 32u
#define AX7Z020_CAPTURE_F_TEST_PATTERN (1u << 0)
#define AX7Z020_CAPTURE_F_SELECTABLE_RATE (1u << 1)
#define AX7Z020_CAPTURE_START_TEST_PATTERN (1u << 0)
/* Rate mask / RTL selector order: 0=50000, 1=25000, 2=10000, 3=5000. */
#define AX7Z020_CAPTURE_RATE_MASK 0x0fu

enum ax7z020_capture_state {
    AX7Z020_CAPTURE_IDLE = 0,
    AX7Z020_CAPTURE_PREPARING = 1,
    AX7Z020_CAPTURE_RUNNING = 2,
    AX7Z020_CAPTURE_READY = 3,
    AX7Z020_CAPTURE_FAILED = 4,
    AX7Z020_CAPTURE_CANCELLED = 5
};

struct ax7z020_capture_caps {
    __u32 abi_version, hardware_id, revision, features;
    __u32 clock_hz, max_records, record_bytes, rate_mask;
    __u32 adc_range_uv, channel_map, speed_filter_cycles, speed_timeout_cycles;
    __u32 reserved[4];
};

struct ax7z020_capture_plan {
    __u32 abi_version, sample_rate_hz, requested_records, flags;
    __u32 reserved[4];
};

struct ax7z020_capture_status {
    __u32 abi_version, state, error_code, sample_rate_hz;
    __u32 requested_records, captured_records, sent_records, dropped_records;
    __u32 hw_status, dma_status, dma_length, flags;
    __u32 reserved[4];
};

#define AX7Z020_CAPTURE_GET_CAPS _IOR('Z', 0x40, struct ax7z020_capture_caps)
#define AX7Z020_CAPTURE_GET_STATUS _IOR('Z', 0x41, struct ax7z020_capture_status)
#define AX7Z020_CAPTURE_START _IOW('Z', 0x42, struct ax7z020_capture_plan)
#define AX7Z020_CAPTURE_CANCEL _IO('Z', 0x43)

#endif
