// shared declarations between ffrdp.c (compiled with -D substitutions) and simnet.c
#ifndef SIMDEFS_H
#define SIMDEFS_H

#include <stddef.h>
#include <stdint.h>

// 仿真里 addr 对协议是不透明的 ✓ —— 但 P2 第二步起协议会**按地址分发** ✓（ffrdp_conn_by_addr ✓），
// 所以这个默认值必须等于协议真算出来的那个地址 ✓：
//   ffrdp_addr_make(127.0.0.1, 8001) = (0x7F000001 << 16) | 8001 = 0x7F0000011F41 ✓
// 仿真里所有测试都用 "127.0.0.1:8001" ✓。**换端口就要同步改这里** ✗（否则帧会被当"别人的包"丢掉 ✓）。
#define SIMLL_ADDR 0x7F0000011F41ULL

#define SIMNET_SLOTS 8   // 最多几个 slot：一个 slot = 一端。v2 的多连接测试里，服务端占一个、每个对端各一个。

uint32_t sim_tick(void);             // the virtual clock: run.sh rewrites utils.c's timestamp into this

void *sim_calloc(size_t n, size_t s);
void  sim_free(void *p);
void  sim_reset(void);
void  sim_begin(int idx);

// ── 多对端（P0）──────────────────────────────────────────────────────────────
// 一个 slot = 一端。登记了 slot 自己的地址（非 0）之后，收发的包才带"谁发的"；
// 不登记（0）= 完全走旧路径，行为与以前逐字相同 —— 老的两端测试一行都不用改。
void     sim_slot(int idx, uint64_t addr);                          // 登记 slot idx 自己的地址
int      sim_send_from(int dst, uint64_t src, void *buf, int len);  // 显式指定：目的 slot + 包里带的源地址
uint64_t sim_recv_src(void);                                        // 最近一次 sim_recv 的包的源地址（0 = 旧路径）

// 假 lowlevel 的按地址路由：登记"对端地址 ⇒ 目的 slot"之后，ll->send(addr) 才会按地址走。
int  sim_ll_route(void *fl, uint64_t addr, int slot);
void sim_ll_own  (void *fl, uint64_t addr);                         // 本实例自己的地址（回给已登记对端时带的源地址）
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
