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
// 可用 -DFFRDP_LOWLEVEL_FRAME_MAX=... 覆盖（嵌入式小 MTU / 内存紧的时候 ✓）：它是每槽载荷的源头，
// 缩它 = 两块环同比缩（1500 → 1000 ⇒ 每槽 1512/1544 → 1016/1040 ✓）。
#ifndef FFRDP_LOWLEVEL_FRAME_MAX
#define FFRDP_LOWLEVEL_FRAME_MAX 1500
#endif

// 这个端点给自己开的收发缓冲大小（字节）：按上面的一帧上限算 128 帧，再给每帧留 6 字节余地 —— 协议层
// 会在一帧外面套几字节（4 字节帧头 + 2 字节 FEC seq），这里不引用它的宏：那是协议的事，传输层只需要
// 一个"装得下一段时间里在途的帧"的数。它跟着实现走（UDP 用一个 socket 缓冲，别的后端可能完全没有）。
#define FFRDP_LOWLEVEL_BUFFER_SIZE (128 * (FFRDP_LOWLEVEL_FRAME_MAX + 6))

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
//    One call per frame, but one lap of the transport's own thread may deliver several of them back to back
//    before the next TIMER: a backend that drains what is ready (the UDP one does, up to a bound) lets the
//    layer above answer a whole burst with a single reply instead of one per frame. A timer armed from
//    inside a receive callback is delivered in that same lap, and that is what makes the batching work.
//  - FFRDP_LOWLEVEL_TYPE_TIMER: buf is NULL and len is 0. Work that came due is done here, and the
//    callback is expected to call timer() again before returning, to say when it wants the next one.
//  - send() and timer() may be called from inside the callback. exit() may not: it joins this thread, so
//    calling it from here is a self join. Do not block in the callback either -- it is the receive path.
typedef long (*FFRDP_LOWLEVEL_CB)(void *cbctx, int type, void *buf, int len);

typedef struct {
    // 0 = handed to the transport, -1 = this frame was not accepted. Either way the frame is gone as far as
    // this layer is concerned; retransmitting it is the protocol's job, not the transport's.
    int  (*send )(void *fl, uint64_t addr, uint8_t *buf, int len); // addr: 32bit ip + 16bit port
    // Ask to be called back (FFRDP_LOWLEVEL_TYPE_TIMER) no later than this absolute millisecond, on the
    // same clock the implementation reports time with (utils.h 的 get_timestamp32_ms). 0 = no deadline, sleep
    // until a frame arrives. A deadline that has already passed means now, which is how a caller hurries
    // the thread up. Each call replaces the previous deadline rather than adding to it.
    // Callable from any thread: the layer above uses it from the application's thread when it has just
    // queued a frame, so that the frame goes out immediately instead of at the deadline set earlier.
    int  (*timer)(void *fl, uint32_t deadline_ms);
    void (*exit )(void *fl);
    // Turn a peer's text form ("1.2.3.4:5678", the backend's own syntax) into an addr that send() accepts,
    // so a layer that was configured with a peer can speak first. An addr taken out of a receive message
    // can simply be echoed back, but a peer that has not spoken yet has to be named some other way, and
    // the layout of the value is private to the backend -- only send() and this call ever look inside it.
    // Returns 0 on success, -1 if the text is not a peer the backend can address.
    // 这里没有"文本 → addr"的槽位：那个转换没有后端差异（addr 的布局是两条层共同的约定，见
    // utils.h 的 ffrdp_addr_make / ffrdp_addr_parse_text），所以它不属于传输层的接口。
} FFRDP_LOWLEVEL;

// params:
//  - if NULL return NULL
//  - "bind:127.0.0.1:1234", indicate bind address and port
//  - "none": do not bind. A client endpoint wants this: the source port is then chosen by the kernel when
//    the first frame goes out, which is what this project did before the transport owned the socket.
FFRDP_LOWLEVEL* ffrdp_udp_lowlevel_init(void *params, FFRDP_LOWLEVEL_CB callback, void *cbctx);

#endif

