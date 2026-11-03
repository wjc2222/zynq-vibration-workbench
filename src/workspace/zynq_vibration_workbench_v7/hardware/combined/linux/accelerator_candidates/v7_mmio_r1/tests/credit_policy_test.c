#include "v7_credit_policy.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t seed = 0x2170fabu;
static unsigned random_value(void) { seed = seed * 1664525u + 1013904223u; return seed; }

int main(void)
{
    unsigned checks = 0, pending, limit, available, bits;
    assert(sizeof(int) == 4 && sizeof(unsigned) == 4);
    for (pending = 0; pending <= 511; ++pending)
        for (limit = 1; limit <= 128; ++limit) {
            unsigned expected = 511 - pending;
            if (expected > limit) expected = limit;
            assert(!v7_accel_credit(82551, 82551-pending, 0, 82551, limit, &available));
            assert(available == expected);
            ++checks;
        }
    assert(v7_accel_credit(5,6,0,1,128,&available));
    assert(v7_accel_credit(6,5,6,1,128,&available));
    assert(v7_accel_credit(512,0,0,1,128,&available));
    assert(v7_accel_credit(5,0,0,1,129,&available));
    for (unsigned trial = 0; trial < 128; ++trial) {
        unsigned issued=0,accepted=0,consumed=0,previous=0,credit=0;
        unsigned total=trial%4==0?82551:trial%4==1?41276:trial%4==2?16511:8256;
        unsigned batch=trial%128+1;
        while (consumed < total) {
            if (!credit && issued < total) {
                unsigned wanted = total-issued < batch ? total-issued : batch;
                assert(!v7_accel_credit(issued,consumed,previous,total-issued,batch,&available));
                previous=consumed;
                if (available >= wanted) credit=wanted;
            }
            if (credit && (random_value()&3)) { ++issued;--credit; }
            /* Issued commits may remain in the interconnect. Count them as
             * occupied before they reach the hardware FIFO. */
            if (accepted < issued && (random_value()&3)) ++accepted;
            assert(accepted-consumed <= 511);
            if (accepted > consumed && (random_value()&3)) ++consumed;
            assert(consumed <= accepted && accepted <= issued && issued <= total);
            if (issued-consumed > 511) {
                /* This includes still-unused credits in addition to issued
                 * writes only if a caller grants past the conservative bound. */
                assert(0);
            }
        }
        assert(issued==total && accepted==total && !credit);
    }
    for(bits=0; bits<0x1000000u; ++bits) {
        unsigned low=bits&0x800000u ? bits|0xff000000u : bits;
        int value, expected=bits&0x800000u ? (int)bits-16777216 : (int)bits;
        assert(!v7_accel_sample32(low,&value) && value==expected);
    }
    for(unsigned i=0;i<4;++i) {
        const unsigned invalid[]={0x00800000u,0x01000000u,0xff000000u,0x80000000u};int value;
        assert(v7_accel_sample32(invalid[i],&value));
    }
    printf("V7_CREDIT_POLICY_PASS exhaustive_credit=%u posted_write_scenarios=128 exact_q7_words=16777216 invalid_counts_and_words=rejected\n",checks);
    return 0;
}
