/* SPDX-License-Identifier: MIT
 * Additive streaming ABI. Existing finite ioctls/structures stay unchanged.
 * All fields are native-endian fixed-width values; raw records stay 32-byte LE.
 */
#ifndef AX7Z020_CAPTURE_STREAM_UAPI_H
#define AX7Z020_CAPTURE_STREAM_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define AX7Z020_STREAM_ABI 1u
#define AX7Z020_STREAM_HW_ID 0x53545231u /* STR1 extension at register 0x70 */
#define AX7Z020_STREAM_RING_CHUNKS 256u
#define AX7Z020_STREAM_CHUNK_PERIOD_US 40000u
#define AX7Z020_STREAM_MAX_CHUNK_RECORDS 2000u
#define AX7Z020_STREAM_MAX_CHUNK_BYTES 64000u
#define AX7Z020_STREAM_F_TIMED (1u << 0)
#define AX7Z020_STREAM_F_BOUNDARY_STOP (1u << 1)
#define AX7Z020_STREAM_F_VERIFIED_READ (1u << 2)
#define AX7Z020_STREAM_START_TEST_PATTERN (1u << 0)
#define AX7Z020_STREAM_STATUS_TEST_PATTERN (1u << 0)
#define AX7Z020_STREAM_STATUS_STOP_REQUESTED (1u << 1)

enum ax7z020_stream_state {
    AX7Z020_STREAM_IDLE = 0,
    AX7Z020_STREAM_PREPARING = 1,
    AX7Z020_STREAM_RUNNING = 2,
    AX7Z020_STREAM_STOPPING = 3,
    AX7Z020_STREAM_READY = 4,
    AX7Z020_STREAM_FAILED = 5,
    AX7Z020_STREAM_CANCELLED = 6
};

struct ax7z020_stream_caps {
    __u32 abi_version, features, clock_hz, record_bytes;
    __u32 rate_mask, chunk_period_us, ring_chunks, max_chunk_records;
    __u32 max_chunk_bytes, max_ring_bytes, reserved[6];
}; /* 64 bytes */

struct ax7z020_stream_plan {
    __u32 abi_version, sample_rate_hz, flags, ring_chunks;
    /* 0 means manual stop. Otherwise positive integer seconds * rate. */
    __aligned_u64 total_records;
    __aligned_u64 reserved[3];
}; /* 48 bytes; ring_chunks must be 0(default) or 256 */

struct ax7z020_stream_status {
    __u32 abi_version, state, error_code, flags;
    __u32 sample_rate_hz, chunk_records, ring_chunks, queued_chunks;
    __aligned_u64 capture_id, total_records, produced_records, sent_records;
    __aligned_u64 committed_records, delivered_records, dropped_records;
    __aligned_u64 first_tick, last_tick, stop_records;
    __aligned_u64 completed_chunks, delivered_chunks;
    __u32 hw_status, dma_status, reserved[6];
}; /* 160 bytes; committed = validated DMA prefix, NOT storage durability */

#define AX7Z020_STREAM_GET_CAPS _IOR('Z', 0x50, struct ax7z020_stream_caps)
#define AX7Z020_STREAM_START _IOW('Z', 0x51, struct ax7z020_stream_plan)
#define AX7Z020_STREAM_STOP _IO('Z', 0x52)
#define AX7Z020_STREAM_GET_STATUS _IOR('Z', 0x53, struct ax7z020_stream_status)

/* read(2) in a stream session returns exactly ONE complete verified chunk.
 * Its records are the untouched LE32-byte payload, carrying global u64 index
 * and tick. Allocate sample_rate_hz/25*32 bytes (at most 64000). A shorter
 * buffer => EMSGSIZE with no consumption; a larger buffer still gets one chunk.
 * A BD is rearmed only after the complete copy_to_user succeeds. Nonblocking
 * read before a committed chunk => EAGAIN. After READY and prefix drained => 0.
 * FAILED/CANCELLED preserve readable committed chunks, then return error_code.
 * poll => readable committed prefix; HUP on drained READY; ERR on failure.
 * STOP is idempotent, drains to the NEXT 40ms boundary and preserves the tail;
 * it does not wait for disk fsync. Legacy CANCEL/close remain abort operations.
 * START after terminal state explicitly discards any unread previous prefix.
 */
#endif
