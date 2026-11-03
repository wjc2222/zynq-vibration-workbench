/* SPDX-License-Identifier: MIT
 * Additive preview ABI for verified SG DMA completion. Base ABI remains intact.
 * Neither physical addresses nor userspace pointers are present in this ABI.
 */
#ifndef AX7Z020_CAPTURE_PREVIEW_UAPI_H
#define AX7Z020_CAPTURE_PREVIEW_UAPI_H
#include "ax7z020_capture.h"

#define AX7Z020_CAPTURE_F_PREVIEW_COMMITTED (1u << 2)
#define AX7Z020_CAPTURE_F_SG_CHAIN (1u << 3)
#define AX7Z020_CAPTURE_PREVIEW_ABI 1u
#define AX7Z020_CAPTURE_PREVIEW_MAX_RECORDS 256u
#define AX7Z020_CAPTURE_PREVIEW_PERIOD_US 40000u
#define AX7Z020_CAPTURE_PREVIEW_LATEST (1u << 0)
#define AX7Z020_CAPTURE_PREVIEW_V_COMMITTED (1u << 0)
#define AX7Z020_CAPTURE_PREVIEW_V_RECORDS_VALID (1u << 1)
#define AX7Z020_CAPTURE_PREVIEW_V_JOB_COMPLETE (1u << 2)
#define AX7Z020_CAPTURE_PREVIEW_V_SOURCE_VERIFIED (1u << 3)
#define AX7Z020_CAPTURE_PREVIEW_V_CRC32 (1u << 4)

/*64 bytes. committed_records is ONLY the contiguous validated BD.Complete
 * prefix, never a PL produced/sent counter. capture_id changes on each START.
 */
struct ax7z020_capture_preview_info {
    __u32 abi_version, state, error_code, source_flags;
    __aligned_u64 capture_id;
    __u32 sample_rate_hz, record_bytes, requested_records, committed_records;
    __u32 chunk_records, completed_chunks, total_chunks, features;
    __u32 committed_crc32, reserved;
};

/*8272 bytes, below Linux's14-bit ioctl size limit. Input: abi_version, flags,
 * first_record, requested_records(1..256), current nonzero capture_id, reserved0.
 * Without LATEST, return records at first_record, up to the committed prefix.
 * LATEST returns the newest requested_records in the committed prefix.
 * RUNNING with no requested committed data => EAGAIN. Wrong capture_id => ESTALE.
 * FAILED/CANCELLED/IDLE => state errno/ENODATA. No mmap or physical address.
 * Output unused payload bytes are zero. payload_crc32 is standard IEEE CRC32
 * over exactly record_count*record_bytes bytes, including original indices,
 * ticks, ADC values/validity and speed fields in unmodified32-byte LE records.
 */
struct ax7z020_capture_preview_copy {
    __u32 abi_version, flags, first_record, requested_records;
    __aligned_u64 capture_id;
    __u32 record_count, committed_records, sample_rate_hz, source_flags;
    __u32 state, error_code, validity_flags, payload_crc32;
    __u32 record_bytes, chunk_records, reserved[4];
    __u8 records[AX7Z020_CAPTURE_PREVIEW_MAX_RECORDS * AX7Z020_CAPTURE_RECORD_BYTES];
};

#define AX7Z020_CAPTURE_GET_PREVIEW_INFO _IOR('Z', 0x44, struct ax7z020_capture_preview_info)
#define AX7Z020_CAPTURE_COPY_PREVIEW _IOWR('Z', 0x45, struct ax7z020_capture_preview_copy)
#endif
