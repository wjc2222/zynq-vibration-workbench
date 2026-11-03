/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef V7_ACCEL_CREDIT_POLICY_H
#define V7_ACCEL_CREDIT_POLICY_H
#define V7_ACCEL_FIFO_USABLE 511u
#define V7_ACCEL_MAX_BATCH 128u
/* issued includes queued/in-flight commits, consumed counts FIFO pops.
 * Therefore issued-consumed >= actual FIFO occupancy, even with posted writes.
 * This function grants only a conservative lower bound on remaining space. */
static inline int v7_accel_credit(unsigned int issued, unsigned int consumed,
                                unsigned int previous_consumed,
                                unsigned int remaining, unsigned int limit,
                                unsigned int *available)
{
    unsigned int pending, free;
    if (!limit || limit > V7_ACCEL_MAX_BATCH || consumed > issued ||
        consumed < previous_consumed)
        return -1;
    pending = issued - consumed;
    if (pending > V7_ACCEL_FIFO_USABLE)
        return -1;
    free = V7_ACCEL_FIFO_USABLE - pending;
    if (free > remaining) free = remaining;
    if (free > limit) free = limit;
    *available = free;
    return 0;
}
static inline int v7_accel_sample32(unsigned int low, int *value)
{
    int signed_value = (int)low;
    if (signed_value < -8388608 || signed_value > 8388607)
        return -1;
    *value = signed_value;
    return 0;
}
#endif
