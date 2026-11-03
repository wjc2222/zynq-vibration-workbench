/* SPDX-License-Identifier: MIT
 * V6 analysis-only ABI. No physical addresses or native-width fields.
 * Exclusive open; write exactly raw_frames*6 bytes (sample-major signed16 LE),
 * RUN synchronously, then read exactly one result. RUN clears the previous
 * result before starting; a failed transaction cannot expose an older result.
 */
#ifndef AX7Z020_ACCEL_UAPI_H
#define AX7Z020_ACCEL_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>
#define AX7Z020_ACCEL_ABI 1u
#define AX7Z020_ACCEL_HW_ID 0x36414e41u
#define AX7Z020_ACCEL_POINTS 8192u
#define AX7Z020_ACCEL_BINS 4097u
#define AX7Z020_ACCEL_ORDERS 13u
#define AX7Z020_ACCEL_MAX_RAW_FRAMES 82560u
#define AX7Z020_ACCEL_PLAN_BANDS_VALID 1u
struct ax7z020_accel_caps {
    __u32 abi_version, hardware_id, revision, clock_hz;
    __u32 points, bins, rate_mask, max_raw_frames;
    __u32 features, reserved[7];
};
struct ax7z020_accel_band { __u32 first, last; };
struct ax7z020_accel_status {
    __u32 abi_version, hardware_id, revision, status;
    __u32 errors, raw_frames, lowband_frames, raw_required;
    __u32 reserved[8];
};
struct ax7z020_accel_plan {
    __u32 abi_version, sample_rate_hz, raw_frames, timeout_ms;
    __u32 flags, reserved[3];
    struct ax7z020_accel_band bands[AX7Z020_ACCEL_ORDERS];
};
struct ax7z020_accel_channel {
    __s64 sum_q7;
    __u64 sumsq_q14;
    __s32 minimum_q7, maximum_q7;
    __u32 fft_exponent, fft_pre_shift;
    __u64 raw_endpoint_count, saturation_count;
    __s32 lowband_q7[AX7Z020_ACCEL_POINTS];
    __u64 fft_magnitude_squared[AX7Z020_ACCEL_BINS];
    /* 0..12 orders,13 reference,14 full0..2k,15 order union,
     * 16 orders4..10 union,17 nonharmonic. One-sided weighted sum:
     * DC/Nyquist weight1, all other bins weight2. */
    __u64 band_magnitude_squared[18];
};
struct ax7z020_accel_result {
    __u32 abi_version, hardware_id, revision, sample_rate_hz;
    __u32 points, fir_taps, decimation, flags;
    __u32 saturation_count, error_code;
    __u64 cycles;
    __u32 reserved[4];
    struct ax7z020_accel_channel channel[3];
};
#define AX7Z020_ACCEL_GET_CAPS _IOR('V', 0x70, struct ax7z020_accel_caps)
#define AX7Z020_ACCEL_RUN _IOW('V', 0x71, struct ax7z020_accel_plan)
#define AX7Z020_ACCEL_RESET _IO('V', 0x72)
#define AX7Z020_ACCEL_GET_STATUS _IOR('V', 0x73, struct ax7z020_accel_status)
#endif
