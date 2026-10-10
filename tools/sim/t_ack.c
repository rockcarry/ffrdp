// deterministic test: do the new ack diagnostics report what they claim to.
// Three ack frames are injected by hand:
//   1) uma=1, mack=0        -> cumulative ack only
//   2) uma=1, mack=0b000111 -> seq 2,3,4 arrived, contiguous, no gap
//   3) uma=1, mack=0b001010 -> seq 3 and 5 arrived, seq 4 missing -> a real gap
// so ack_recv=3, ack_mack=2, ack_hole=1 is the expected result.
#include <stdio.h>
#include <string.h>
#include "simdefs.h"
#include "../../ffrdp.c"

static void make_ack(unsigned char *ack, unsigned int una, unsigned int mack, unsigned char rwnd)
{
    memset(ack, 0, 8);
    *(uint32_t*)(ack + 0) = (FFRDP_FRAME_TYPE_ACK << 0) | (una << 8);
    *(uint32_t*)(ack + 4) = (mack << 0) | ((uint32_t)rwnd << 24);
}

int main(void)
{
    static unsigned char buf[64 * 1024], ack[8];
    FFRDPCONTEXT *c;

    sim_reset();
    sim_config(0, 0, 0, 0);
    c = ffrdp_init("127.0.0.1", 8001, NULL, NULL, 0, 1024, 0);
    sim_begin(0);
    ffrdp_send(c, (char*)buf, 8 * 1024);
    ffrdp_update(c);

    make_ack(ack, 1, 0x00, 255); sim_inject(0, ack, 8); ffrdp_update(c);
    make_ack(ack, 1, 0x07, 255); sim_inject(0, ack, 8); ffrdp_update(c);
    make_ack(ack, 1, 0x0a, 255); sim_inject(0, ack, 8); ffrdp_update(c);

#ifdef NEW_COUNTERS
    printf("  ack_recv=%u ack_mack=%u ack_hole=%u (expect 3/2/1) -> %s\n",
           c->counter_ack_recv, c->counter_ack_mack, c->counter_ack_hole,
           (c->counter_ack_recv == 3 && c->counter_ack_mack == 2 && c->counter_ack_hole == 1) ? "PASS" : "FAIL");
#ifdef OOO_HIST
    // the masked bytes are counted by population, not by frame: 0x07 has three bits set, 0x0a has two,
    // so a sender that counted what it actually received has to read five.
    printf("  mack_bits_recv=%u (expect 5 = 3 from 0x07 + 2 from 0x0a) -> %s\n",
           c->counter_mack_bits_recv, c->counter_mack_bits_recv == 5 ? "PASS" : "FAIL");
    if (c->counter_mack_bits_recv != 5) return 1;
#endif
    return (c->counter_ack_recv == 3 && c->counter_ack_mack == 2 && c->counter_ack_hole == 1) ? 0 : 1;
#else
    printf("  counter_ack_* does not exist in this build\n");
    return 0;
#endif
}
