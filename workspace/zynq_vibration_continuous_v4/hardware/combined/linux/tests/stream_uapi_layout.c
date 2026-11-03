/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "ax7z020_capture_stream.h"
#define CHECK(expr) _Static_assert((expr), #expr)
CHECK(sizeof(struct ax7z020_stream_caps) == 64);
CHECK(sizeof(struct ax7z020_stream_plan) == 48);
CHECK(sizeof(struct ax7z020_stream_status) == 160);
CHECK(offsetof(struct ax7z020_stream_plan, total_records) == 16);
CHECK(offsetof(struct ax7z020_stream_status, capture_id) == 32);
CHECK(offsetof(struct ax7z020_stream_status, produced_records) == 48);
CHECK(offsetof(struct ax7z020_stream_status, committed_records) == 64);
CHECK(offsetof(struct ax7z020_stream_status, first_tick) == 88);
CHECK(offsetof(struct ax7z020_stream_status, stop_records) == 104);
CHECK(offsetof(struct ax7z020_stream_status, hw_status) == 128);
CHECK(_IOC_SIZE(AX7Z020_STREAM_GET_CAPS) == 64);
CHECK(_IOC_SIZE(AX7Z020_STREAM_START) == 48);
CHECK(_IOC_SIZE(AX7Z020_STREAM_GET_STATUS) == 160);
CHECK(_IOC_SIZE(AX7Z020_STREAM_STOP) == 0);
CHECK(AX7Z020_STREAM_RING_CHUNKS * AX7Z020_STREAM_MAX_CHUNK_BYTES == 16384000);
int main(void) { return 0; }
