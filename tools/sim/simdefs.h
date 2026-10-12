// shared declarations between ffrdp.c (compiled with -D substitutions) and simnet.c
#ifndef SIMDEFS_H
#define SIMDEFS_H

#include <stddef.h>
#include <stdint.h>

uint32_t sim_tick(void);             // the virtual clock: run.sh rewrites utils.c's timestamp into this

void *sim_calloc(size_t n, size_t s);
void  sim_free(void *p);
void  sim_reset(void);
void  sim_begin(int idx);
void  sim_config(double loss, int burst, int delay, int jitter);
void  sim_shaper(double bytes_per_ms, int queue_bytes); // shared ack/data shaper, like an AP policing udp
void  sim_policer(double bytes_per_ms, int burst_bytes); // token bucket on the direction sent by side 0, drops the burst excess
void  sim_spike(int percent, int extra_ms);             // heavy tailed delay: a retry storm delays everything behind it
void  sim_setstep(int step);
void  sim_advance(uint32_t ms);      // jump the virtual clock forward
void  sim_inject(int idx, void *buf, int len); // queue a raw packet for one side
int   sim_queued(int idx);
void  sim_ll_run(int idx, void *ctxt); // drive the fake lowlevel: one RECV per queued packet, then one TIMER
unsigned long sim_alloc(void);
unsigned long sim_freed(void);

#endif
