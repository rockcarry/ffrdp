#ifndef __FFRDP_LOWLEVEL_H__
#define __FFRDP_LOWLEVEL_H__

#include <stdint.h>

// The largest frame this interface carries, in bytes. It belongs to the interface rather than to UDP: it is
// how much a backend can receive into one buffer, and therefore the ceiling the layer above must stay under
// when it hands a frame over -- a receive beyond it is truncated, silently, on purpose. A backend that can
// carry something else says so with its own value (a serial link would have a much smaller one), which is
// why the layer above should size its frames from this constant rather than from a number of its own: on
// UDP it is the Ethernet sized limit this project is built on, and FFRDP_MAX_MSS is 1500 - 8, leaving room
// for the UDP header on top.
#define FFRDP_LOWLEVEL_FRAME_MAX 1500

enum {
    FFRDP_LOWLEVEL_TYPE_RECV  = 1,
    FFRDP_LOWLEVEL_TYPE_TIMER = 2,
};

typedef struct {
    uint64_t addr; // 32bit ip + 16bit port
    void    *buf;
    int      len;
} FFRDP_LOWLEVEL_MSG;

// A lowlevel is a transport endpoint that owns a thread: it sends frames, receives frames, and reports
// ticks. Those three are the whole of what the layer above needs (tx, rx, timer), and they are what lets
// it stop knowing about sockets. The thread is part of the concept rather than an implementation detail:
// it is what allows the protocol above to be driven by the transport instead of by the application's loop.
//
// The callback runs on that thread, one call at a time, never entered from inside another:
//  - FFRDP_LOWLEVEL_TYPE_RECV: buf is a FFRDP_LOWLEVEL_MSG and len is sizeof(FFRDP_LOWLEVEL_MSG). The
//    message's buf points at the lowlevel's own buffer and is valid only for the duration of this call, so
//    copy what has to be kept. Handing msg->addr back to send() is how a reply is addressed.
//  - FFRDP_LOWLEVEL_TYPE_TIMER: buf is NULL and len is 0. Work that came due is done here, and the
//    callback is expected to call timer() again before returning, to say when it wants the next one.
//  - send() and timer() may be called from inside the callback. exit() may not: it joins this thread, so
//    calling it from here is a self join. Do not block in the callback either -- it is the receive path.
typedef long (*FFRDP_LOWLEVEL_CB)(void *cbctx, int type, void *buf, int len);

typedef struct {
    // 0 = handed to the transport, -1 = this frame was not accepted. Either way the frame is gone as far as
    // this layer is concerned; retransmitting it is the protocol's job, not the transport's.
    int  (*send)(void *fl, uint64_t addr, uint8_t *buf, int len); // addr: 32bit ip + 16bit port
    // Ask to be called back (FFRDP_LOWLEVEL_TYPE_TIMER) no later than this absolute millisecond, on the
    // same clock the implementation reports time with (ffrdp_get_timestamp32_ms). 0 = no deadline, sleep
    // until a frame arrives. A deadline that has already passed means now, which is how a caller hurries
    // the thread up. Each call replaces the previous deadline rather than adding to it.
    // Callable from any thread: the layer above uses it from the application's thread when it has just
    // queued a frame, so that the frame goes out immediately instead of at the deadline set earlier.
    int  (*timer)(void *fl, uint32_t deadline_ms);
    void (*exit)(void *fl);
} FFRDP_LOWLEVEL;

// params:
//  - if NULL return NULL
//  - "bind:127.0.0.1:1234", indicate bind address and port
FFRDP_LOWLEVEL* ffrdp_udp_lowlevel_init(void *params, FFRDP_LOWLEVEL_CB callback, void *cbctx);

#endif

