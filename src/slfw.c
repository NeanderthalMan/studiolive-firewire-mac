// SPDX-License-Identifier: GPL-2.0-or-later
#include "slfw.h"

#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <libkern/OSByteOrder.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- addresses */

#define CSR_ROM_BASE    0xfffff0000400ULL
#define DICE_SPACE      0xffffe0000000ULL
#define PRESONUS_OUI    0x000a92

#define G_NICK_NAME     0x0c
#define G_CLOCK_SELECT  0x4c
#define G_ENABLE        0x50
#define G_STATUS        0x54
#define TX_BLOCK0       0x08
#define TX_ISOC         0x00
#define TX_N_AUDIO      0x04
#define TX_N_MIDI       0x08
#define TX_NAMES        0x10

static const unsigned rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000 };

/* --------------------------------------------------------------- the rings
 *
 * Packet ring: 4096 slots is 512 ms at 8000 packets/s, so a call-back can be
 * that late before audio is lost — and a loss is detected from DBC, not hoped
 * against. 2048 bytes a slot holds a 48 kHz packet (1288) with room.
 *
 * Frame ring: 2^16 frames is 1.36 s at 48 kHz. A reader that falls further
 * behind than that gets zeros and a count, never torn samples. */
#define NPKT         4096
#define PKTSZ        2048
#define GROUP        32
#define RING_FRAMES  (1u << 16)
#define READ_MARGIN  4096     /* frames a writer may overwrite before publishing */

/* Clock fit: the earliest arrival in each half-second window becomes a point;
 * the line through the last 64 points (32 s) is the device's timeline. Minima
 * hug the low-jitter envelope; a 32 s baseline makes the rate precise enough
 * that the line barely moves as points slide. */
#define CLK_POINTS   64

struct slfw {
    slfw_log_fn                     log;
    CFRunLoopRef                    rl;
    IOCFPlugInInterface           **plug;
    IOFireWireLibDeviceRef          dev;
    io_object_t                     svc;
    int                             swap;
    /* What slfw_open got as far as, so a failed open undoes only that. Removing
     * dispatchers that were never added traps in CFRelease. */
    bool                            opened, dispatched;
    uint64_t                        gbase, tbase;
    slfw_info                       info;

    uint8_t                        *pktbuf;
    IOFireWireLibDCLCommandPoolRef  pool;
    DCLCommand                     *program;
    IOFireWireLibLocalIsochPortRef  local;
    IOFireWireLibRemoteIsochPortRef remote;
    IOFireWireLibIsochChannelRef    chan;
    uint32_t                        irm_channel;
    bool                            enabled;
    _Atomic bool                    running, force_stopped;

    int32_t                        *ring;
    _Atomic uint64_t                wpos;
    bool                            have_dbc;
    unsigned                        expect_dbc;
    bool                            fdf_checked;

    /* clock — written on the run-loop thread, read under a seqlock */
    double                          nom_tpf;
    uint64_t                        org_frame;
    double                          org_host;
    struct { double x, y; }         pts[CLK_POINTS];
    unsigned                        npts, pthead;
    uint64_t                        win_end;
    double                          win_best_resid, win_best_x, win_best_y;
    bool                            win_have;
    _Atomic uint32_t                clk_seq;
    double                          clk_tpf, clk_base_host;
    uint64_t                        clk_base_frame;
    bool                            clk_valid;
    _Atomic uint32_t                seed;

    _Atomic uint64_t                callbacks, packets, data_packets, bad_packets,
                                    dbc_breaks, frames_lost;
};

static slfw *g_self;

static void lg(slfw *s, const char *fmt, ...)
{
    if (!s || !s->log) return;
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    s->log(b);
}

/* ------------------------------------------------------------- register I/O */

static IOReturn rd(slfw *s, uint64_t a, uint32_t *v)
{
    FWAddress f = { 0, (UInt16)(a >> 32), (UInt32)(a & 0xffffffffULL) };
    UInt32 raw = 0;
    IOReturn r = (*s->dev)->ReadQuadlet(s->dev, s->svc, &f, &raw, false, 0);
    if (r == kIOReturnSuccess) *v = s->swap ? OSSwapInt32(raw) : raw;
    return r;
}

static IOReturn wr(slfw *s, uint64_t a, uint32_t v)
{
    FWAddress f = { 0, (UInt16)(a >> 32), (UInt32)(a & 0xffffffffULL) };
    UInt32 raw = s->swap ? OSSwapInt32(v) : v;
    return (*s->dev)->WriteQuadlet(s->dev, s->svc, &f, raw, false, 0);
}

/* DICE strings are quadlets whose bytes run low-to-high in text order. */
static void rdstr(slfw *s, uint64_t base, unsigned nbytes, char *out, size_t outsz)
{
    size_t k = 0;
    for (unsigned i = 0; i < nbytes / 4 && k + 4 < outsz; i++) {
        uint32_t v = 0;
        if (rd(s, base + (uint64_t)i * 4, &v) != kIOReturnSuccess) break;
        out[k++] = v & 0xff; out[k++] = (v >> 8) & 0xff;
        out[k++] = (v >> 16) & 0xff; out[k++] = (v >> 24) & 0xff;
    }
    out[k < outsz ? k : outsz - 1] = 0;
}

/* The one place the console is told to be quiet. Safe to call repeatedly. */
static void quiesce(slfw *s)
{
    if (!s || !s->dev || !s->opened) return;
    if (s->gbase) wr(s, s->gbase + G_ENABLE, 0);
    if (s->tbase) wr(s, s->tbase + TX_BLOCK0 + TX_ISOC, 0xffffffff);
    s->enabled = false;
}

/* ---------------------------------------------------------------- the clock */

static uint32_t clk_nom_seed = 1;

static void clock_reset(slfw *s)
{
    s->npts = s->pthead = 0;
    s->win_have = false;
    s->win_end = 0;
    s->org_frame = 0;
    s->org_host = 0;
    atomic_fetch_add(&s->clk_seq, 1);
    s->clk_valid = false;
    atomic_fetch_add(&s->clk_seq, 1);
    atomic_store(&s->seed, clk_nom_seed++);
}

static void clock_publish(slfw *s, double tpf, uint64_t base_frame, double base_host)
{
    atomic_fetch_add(&s->clk_seq, 1);            /* odd: write in progress */
    s->clk_tpf = tpf;
    s->clk_base_frame = base_frame;
    s->clk_base_host = base_host;
    s->clk_valid = true;
    atomic_fetch_add(&s->clk_seq, 1);            /* even: consistent */
}

static void clock_refit(slfw *s)
{
    /* Least squares over the window minima, in origin-relative coordinates so
     * doubles keep their precision for a session measured in hours. */
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    unsigned n = s->npts;
    for (unsigned i = 0; i < n; i++) {
        sx += s->pts[i].x; sy += s->pts[i].y;
        sxx += s->pts[i].x * s->pts[i].x; sxy += s->pts[i].x * s->pts[i].y;
    }
    double tpf = s->nom_tpf, c;
    double den = n * sxx - sx * sx;
    if (n >= 4 && den > 0) {
        tpf = (n * sxy - sx * sy) / den;
        static unsigned refits;
        if (++refits <= 2 || refits % 20 == 0)
            lg(s, "clock fit n=%u raw %+.1f ppm (slope vs nominal)%s", n,
               (s->nom_tpf / tpf - 1) * 1e6,
               (tpf < s->nom_tpf * 0.999 || tpf > s->nom_tpf * 1.001) ? " CLAMPED" : "");
        /* The check that caught the rate bug: a real clock pair is tens of ppm
         * apart. Percent means the nominal rate itself is wrong. */
        static bool warned;
        if (n >= 8 && !warned && (tpf < s->nom_tpf * 0.99 || tpf > s->nom_tpf * 1.01)) {
            warned = true;
            lg(s, "MEASURED RATE %.0f Hz disagrees with the console's %u Hz by over 1%%",
               s->info.sample_rate * s->nom_tpf / tpf, s->info.sample_rate);
        }
        /* A fit more than 0.1% from nominal is noise from a short baseline,
         * not a real clock; a crystal is tens of ppm off, not thousands. */
        if (tpf < s->nom_tpf * 0.999 || tpf > s->nom_tpf * 1.001) tpf = s->nom_tpf;
    }
    c = (sy - tpf * sx) / n;
    clock_publish(s, tpf, s->org_frame, s->org_host + c);
}

static void clock_observe(slfw *s, uint64_t frame, uint64_t now)
{
    if (frame == 0) return;
    if (s->org_host == 0) {
        s->org_frame = frame;
        s->org_host = (double)now;
        s->win_end = frame + s->info.sample_rate / 2;
        clock_publish(s, s->nom_tpf, frame, (double)now);
        return;
    }
    double x = (double)(frame - s->org_frame);
    double y = (double)now - s->org_host;
    double tpf = s->clk_valid ? s->clk_tpf : s->nom_tpf;
    double resid = y - x * tpf;
    if (!s->win_have || resid < s->win_best_resid) {
        s->win_best_resid = resid; s->win_best_x = x; s->win_best_y = y; s->win_have = true;
    }
    if (frame >= s->win_end) {
        s->pts[s->pthead].x = s->win_best_x;
        s->pts[s->pthead].y = s->win_best_y;
        s->pthead = (s->pthead + 1) % CLK_POINTS;
        if (s->npts < CLK_POINTS) s->npts++;
        s->win_have = false;
        s->win_end = frame + s->info.sample_rate / 2;
        clock_refit(s);
    }
}

bool slfw_clock(slfw *s, double *tpf, uint64_t *base_frame, double *base_host, uint32_t *seed)
{
    for (int tries = 0; tries < 8; tries++) {
        uint32_t a = atomic_load(&s->clk_seq);
        if (a & 1) continue;
        bool valid = s->clk_valid;
        double t = s->clk_tpf, h = s->clk_base_host;
        uint64_t f = s->clk_base_frame;
        if (atomic_load(&s->clk_seq) != a) continue;
        if (!valid) return false;
        *tpf = t; *base_frame = f; *base_host = h;
        if (seed) *seed = atomic_load(&s->seed);
        return true;
    }
    return false;
}

/* ----------------------------------------------------------- depacketising */

static void emit_silence(slfw *s, unsigned frames)
{
    uint64_t w = atomic_load(&s->wpos);
    unsigned ch = s->info.channels;
    if (frames > RING_FRAMES) frames = RING_FRAMES;
    for (unsigned i = 0; i < frames; i++, w++)
        memset(s->ring + (w % RING_FRAMES) * ch, 0, ch * sizeof(int32_t));
    atomic_store(&s->wpos, w);
}

static void process_slot(slfw *s, unsigned slot)
{
    UInt32 *q = (UInt32 *)(s->pktbuf + (size_t)slot * PKTSZ);
    UInt32 ih = q[0];                          /* host order, as OHCI writes it */
    unsigned len = ih >> 16, tcode = (ih >> 4) & 0xf;
    if (tcode != 0xa || len < 8 || len > PKTSZ - 4) { atomic_fetch_add(&s->bad_packets, 1); return; }

    UInt32 c1 = OSSwapBigToHostInt32(q[1]);
    UInt32 c2 = OSSwapBigToHostInt32(q[2]);
    unsigned dbs = (c1 >> 16) & 0xff, dbc = c1 & 0xff;
    unsigned fmt = (c2 >> 24) & 0x3f, fdf = (c2 >> 16) & 0xff;
    if (fmt != 0x10 || dbs == 0) { atomic_fetch_add(&s->bad_packets, 1); return; }
    atomic_fetch_add(&s->packets, 1);

    unsigned blocks = (len - 8) / (dbs * 4);

    /* An empty packet carries the DBC of the next data block, so the expected
     * value is always the previous DBC plus the previous block count. A gap is
     * lost audio: count it, and keep the timeline honest by filling it. */
    if (s->have_dbc && dbc != s->expect_dbc) {
        unsigned gap = (dbc - s->expect_dbc) & 0xff;
        atomic_fetch_add(&s->dbc_breaks, 1);
        atomic_fetch_add(&s->frames_lost, gap);
        emit_silence(s, gap);
    }
    s->expect_dbc = (dbc + blocks) & 0xff;
    s->have_dbc = true;
    if (!blocks) return;

    atomic_fetch_add(&s->data_packets, 1);
    if (!s->fdf_checked) {
        unsigned sfc = fdf & 0x07;
        s->fdf_checked = true;
        if (sfc < 7 && rates[sfc] != s->info.sample_rate)
            lg(s, "stream FDF claims %u Hz, console status says %u Hz — using status",
               rates[sfc], s->info.sample_rate);
    }

    unsigned ch = s->info.channels, nch = dbs < ch ? dbs : ch;
    uint64_t w = atomic_load(&s->wpos);
    for (unsigned b = 0; b < blocks; b++, w++) {
        int32_t *dst = s->ring + (w % RING_FRAMES) * ch;
        UInt32 *blk = q + 3 + b * dbs;
        /* The label byte is shifted out and never checked: this console sends
         * 0x00 where IEC 61883-6 says 0x40, and Linux's capture path does the
         * same shift for the same reason. */
        for (unsigned c = 0; c < nch; c++)
            dst[c] = (int32_t)(OSSwapBigToHostInt32(blk[c]) << 8);
        for (unsigned c = nch; c < ch; c++) dst[c] = 0;
    }
    atomic_store(&s->wpos, w);
}

static void dcl_proc(DCLCommand *cmd)
{
    slfw *s = g_self;
    if (!s || !atomic_load(&s->running)) return;
    unsigned end = (unsigned)(uintptr_t)((DCLCallProc *)cmd)->procData;
    atomic_fetch_add(&s->callbacks, 1);
    for (unsigned i = end + 1 - GROUP; i <= end; i++) process_slot(s, i);
    clock_observe(s, atomic_load(&s->wpos), mach_absolute_time());
}

uint64_t slfw_frames(slfw *s) { return atomic_load(&s->wpos); }

unsigned slfw_read(slfw *s, uint64_t pos, int32_t *dst, unsigned n)
{
    unsigned ch = s->info.channels, real = 0;
    uint64_t w = atomic_load(&s->wpos);
    for (unsigned i = 0; i < n; i++) {
        uint64_t f = pos + i;
        int32_t *out = dst + (size_t)i * ch;
        if (f < w && f + RING_FRAMES > w + READ_MARGIN) {
            memcpy(out, s->ring + (f % RING_FRAMES) * ch, ch * sizeof(int32_t));
            real++;
        } else {
            memset(out, 0, ch * sizeof(int32_t));
        }
    }
    /* Validate after copying: anything the writer could have reached while we
     * copied is discarded rather than trusted. */
    uint64_t w2 = atomic_load(&s->wpos);
    if (w2 != w) {
        for (unsigned i = 0; i < n; i++) {
            uint64_t f = pos + i;
            if (f < w && !(f + RING_FRAMES > w2 + READ_MARGIN)) {
                memset(dst + (size_t)i * ch, 0, ch * sizeof(int32_t));
                real--;
            }
        }
    }
    return real;
}

void slfw_get_stats(slfw *s, slfw_stats *o)
{
    o->callbacks     = atomic_load(&s->callbacks);
    o->packets       = atomic_load(&s->packets);
    o->data_packets  = atomic_load(&s->data_packets);
    o->bad_packets   = atomic_load(&s->bad_packets);
    o->frames        = atomic_load(&s->wpos);
    o->dbc_breaks    = atomic_load(&s->dbc_breaks);
    o->frames_lost   = atomic_load(&s->frames_lost);
    o->running       = atomic_load(&s->running);
    o->force_stopped = atomic_load(&s->force_stopped);
}

/* ------------------------------------------------------ remote port handlers */

static IOReturn rp_supported(IOFireWireLibIsochPortRef p, IOFWSpeed *speed, UInt64 *chans)
{
    (void)p; *speed = kFWSpeed400MBit; *chans = ~(UInt64)0;
    return kIOReturnSuccess;
}

static IOReturn rp_allocate(IOFireWireLibIsochPortRef p, IOFWSpeed speed, UInt32 chan)
{
    (void)p; (void)speed;
    slfw *s = g_self;
    s->irm_channel = chan;
    IOReturn r = wr(s, s->tbase + TX_BLOCK0 + TX_ISOC, chan);
    lg(s, "isoch channel %u allocated; tx[0] programmed (0x%08x)", chan, r);
    return r;
}

static IOReturn rp_release(IOFireWireLibIsochPortRef p)
{
    (void)p;
    slfw *s = g_self;
    wr(s, s->tbase + TX_BLOCK0 + TX_ISOC, 0xffffffff);
    return kIOReturnSuccess;
}

static IOReturn rp_start(IOFireWireLibIsochPortRef p)
{
    (void)p;
    slfw *s = g_self;
    IOReturn r = wr(s, s->gbase + G_ENABLE, 1);
    if (r == kIOReturnSuccess) s->enabled = true;
    return r;
}

static IOReturn rp_stop(IOFireWireLibIsochPortRef p)
{
    (void)p;
    slfw *s = g_self;
    wr(s, s->gbase + G_ENABLE, 0);
    s->enabled = false;
    return kIOReturnSuccess;
}

/* A bus reset or an unplug stops the channel underneath us. The console may be
 * gone, so this marks the stream dead and lets the owner decide to restart. */
static void force_stop(IOFireWireLibIsochChannelRef ch, UInt32 cond)
{
    (void)ch;
    slfw *s = g_self;
    if (!s) return;
    atomic_store(&s->running, false);
    atomic_store(&s->force_stopped, true);
    lg(s, "isoch channel force-stopped (condition %u) — bus reset or unplug", cond);
}

/* ------------------------------------------------------------------ discovery */

static uint64_t prop_u64(io_service_t svc, CFStringRef key)
{
    uint64_t v = 0;
    CFTypeRef p = IORegistryEntryCreateCFProperty(svc, key, kCFAllocatorDefault, 0);
    if (p) {
        if (CFGetTypeID(p) == CFNumberGetTypeID()) CFNumberGetValue(p, kCFNumberSInt64Type, &v);
        CFRelease(p);
    }
    return v;
}

io_service_t slfw_find_service(uint64_t guid)
{
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOFireWireDevice"),
                                     &it) != KERN_SUCCESS)
        return 0;
    io_service_t svc, found = 0;
    while ((svc = IOIteratorNext(it))) {
        uint64_t g = prop_u64(svc, CFSTR("GUID"));
        bool ok = guid ? (g == guid) : ((g >> 40) == PRESONUS_OUI);
        if (ok && !found) found = svc; else IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return found;
}

/* ---------------------------------------------------------------- lifecycle */

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); lg(s, "%s", err); slfw_close(s); return NULL; } while (0)

slfw *slfw_open(io_service_t svc, CFRunLoopRef rl, slfw_log_fn log, char *err, size_t errlen)
{
    if (g_self) { snprintf(err, errlen, "a console is already open in this process"); return NULL; }
    slfw *s = calloc(1, sizeof *s);
    if (!s) { snprintf(err, errlen, "out of memory"); return NULL; }
    s->log = log;
    s->rl = rl;
    g_self = s;

    SInt32 score;
    if (IOCreatePlugInInterfaceForService(svc, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID,
                                          &s->plug, &score) != kIOReturnSuccess || !s->plug)
        FAIL("could not create the IOFireWireLib plug-in (sandbox, or device gone)");
    if ((*s->plug)->QueryInterface(s->plug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9),
                                   (void **)&s->dev) != S_OK || !s->dev)
        FAIL("no IOFireWireLib v9 device interface");
    IOReturn r = (*s->dev)->Open(s->dev);
    if (r != kIOReturnSuccess) FAIL("Open failed: 0x%08x", r);
    s->opened = true;
    s->svc = (*s->dev)->GetDevice(s->dev);

    /* Before any isoch object: a local port binds its call-back path when it
     * is created (spike/isoprobe, first run). */
    (*s->dev)->AddCallbackDispatcherToRunLoop(s->dev, rl);
    (*s->dev)->AddIsochCallbackDispatcherToRunLoop(s->dev, rl);
    s->dispatched = true;

    FWAddress f = { 0, 0xffff, 0xf0000404 };
    UInt32 magic = 0;
    if ((*s->dev)->ReadQuadlet(s->dev, s->svc, &f, &magic, false, 0) != kIOReturnSuccess)
        FAIL("Configuration ROM unreadable");
    if (magic == 0x31333934) s->swap = 0;
    else if (OSSwapInt32(magic) == 0x31333934) s->swap = 1;
    else FAIL("ROM magic 0x%08x is not 1394", magic);

    uint32_t goff = 0, toff = 0, v = 0;
    rd(s, DICE_SPACE + 0x00, &goff);
    rd(s, DICE_SPACE + 0x08, &toff);
    if (!goff || goff > 0x10000 || toff <= goff) FAIL("not a DICE device (offsets %u/%u)", goff, toff);
    s->gbase = DICE_SPACE + (uint64_t)goff * 4;
    s->tbase = DICE_SPACE + (uint64_t)toff * 4;

    s->info.guid = prop_u64(svc, CFSTR("GUID"));
    rdstr(s, s->gbase + G_NICK_NAME, 64, s->info.nickname, sizeof s->info.nickname);

    rd(s, s->tbase + TX_BLOCK0 + TX_N_AUDIO, &v);
    if (v == 0 || v > SLFW_MAX_CH) FAIL("tx[0] reports %u channels", v);
    s->info.channels = v;

    /* The rate the console is LOCKED to is in GLOBAL_STATUS. clock_select is
     * only what was requested, and the stream's CIP FDF repeats the request:
     * on this console both say 48 kHz while it runs at 44.1 kHz. Proven three
     * ways — data packets are 68.9% of the stream (5512.5/8000, not 6000/8000),
     * 30 s of wall time yields 30.0 s of frames only at 44.1 kHz, and the clock
     * fit against the Mac sits at -81,278 ppm from 48 kHz. clock_select is the
     * fallback only when status is unreadable. */
    s->info.sample_rate = 44100;
    if (rd(s, s->gbase + G_STATUS, &v) == kIOReturnSuccess && ((v >> 8) & 0xff) < 7)
        s->info.sample_rate = rates[(v >> 8) & 0xff];
    else if (rd(s, s->gbase + G_CLOCK_SELECT, &v) == kIOReturnSuccess && ((v >> 8) & 0xff) < 7)
        s->info.sample_rate = rates[(v >> 8) & 0xff];

    char names[260];
    rdstr(s, s->tbase + TX_BLOCK0 + TX_NAMES, 256, names, sizeof names);
    unsigned idx = 0;
    char *p = names, *tok;
    while (idx < s->info.channels && (tok = strsep(&p, "\\")) && *tok)
        snprintf(s->info.names[idx++], SLFW_NAME_LEN, "%s", tok);
    for (; idx < s->info.channels; idx++)
        snprintf(s->info.names[idx], SLFW_NAME_LEN, "Ch %u", idx + 1);

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    s->nom_tpf = (1e9 / s->info.sample_rate) * (double)tb.denom / (double)tb.numer;

    s->ring = calloc((size_t)RING_FRAMES * s->info.channels, sizeof(int32_t));
    s->pktbuf = valloc((size_t)NPKT * PKTSZ);
    if (!s->ring || !s->pktbuf) FAIL("buffer allocation failed");
    memset(s->pktbuf, 0, (size_t)NPKT * PKTSZ);

    s->pool = (*s->dev)->CreateDCLCommandPool(s->dev, (IOByteCount)NPKT * 64 + 8192,
                  CFUUIDGetUUIDBytes(kIOFireWireDCLCommandPoolInterfaceID));
    if (!s->pool) FAIL("CreateDCLCommandPool failed");
    DCLCommand *dcl = s->program = (*s->pool)->AllocateLabelDCL(s->pool, NULL);
    dcl = dcl ? (*s->pool)->AllocateSetTagSyncBitsDCL(s->pool, dcl, 0xF, 0x0) : NULL;
    for (unsigned i = 0; dcl && i < NPKT; i++) {
        dcl = (*s->pool)->AllocateReceivePacketStartDCL(s->pool, dcl, s->pktbuf + (size_t)i * PKTSZ, PKTSZ);
        if (dcl && (i + 1) % GROUP == 0)
            dcl = (*s->pool)->AllocateCallProcDCL(s->pool, dcl, dcl_proc, (DCLCallProcDataType)(uintptr_t)i);
    }
    dcl = dcl ? (*s->pool)->AllocateJumpDCL(s->pool, dcl, (DCLLabel *)s->program) : NULL;
    if (!dcl) FAIL("building the DCL program failed");

    IOVirtualRange range = { (IOVirtualAddress)s->pktbuf, (IOByteCount)NPKT * PKTSZ };
    s->local = (*s->dev)->CreateLocalIsochPort(s->dev, false, s->program, kFWDCLImmediateEvent, 0, 0,
                   NULL, 0, &range, 1, CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!s->local) FAIL("CreateLocalIsochPort failed");

    s->remote = (*s->dev)->CreateRemoteIsochPort(s->dev, true,
                    CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!s->remote) FAIL("CreateRemoteIsochPort failed");
    (*s->remote)->SetGetSupportedHandler(s->remote, rp_supported);
    (*s->remote)->SetAllocatePortHandler(s->remote, rp_allocate);
    (*s->remote)->SetReleasePortHandler(s->remote, rp_release);
    (*s->remote)->SetStartHandler(s->remote, rp_start);
    (*s->remote)->SetStopHandler(s->remote, rp_stop);

    unsigned blocks = s->info.sample_rate > 48000 ? 16 : 8;
    s->chan = (*s->dev)->CreateIsochChannel(s->dev, true, 8 + blocks * s->info.channels * 4,
                  kFWSpeed400MBit, CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!s->chan) FAIL("CreateIsochChannel failed");
    (*s->chan)->SetTalker(s->chan, (IOFireWireLibIsochPortRef)s->remote);
    (*s->chan)->AddListener(s->chan, (IOFireWireLibIsochPortRef)s->local);
    (*s->chan)->SetChannelForceStopHandler(s->chan, force_stop);
    (*s->chan)->TurnOnNotification(s->chan);

    /* A previous owner that died mid-stream could have left it talking. */
    quiesce(s);

    lg(s, "opened %s (GUID %012llx): %u channels, %u Hz", s->info.nickname,
       s->info.guid, s->info.channels, s->info.sample_rate);
    return s;
}

const slfw_info *slfw_get_info(slfw *s) { return &s->info; }

void slfw_override_rate(slfw *s, unsigned rate)
{
    if (!s || !rate || rate == s->info.sample_rate) return;
    lg(s, "rate overridden to %u Hz (console status said %u Hz)", rate, s->info.sample_rate);
    s->info.sample_rate = rate;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    s->nom_tpf = (1e9 / rate) * (double)tb.denom / (double)tb.numer;
    /* The fit was clamped to the wrong nominal; starting it over is cheaper and
     * more honest than steering a line that was never right. */
    clock_reset(s);
}

int slfw_start(slfw *s)
{
    if (atomic_load(&s->running)) return 0;
    memset(s->pktbuf, 0, (size_t)NPKT * PKTSZ);
    atomic_store(&s->wpos, 0);
    s->have_dbc = false;
    s->fdf_checked = false;
    atomic_store(&s->force_stopped, false);
    clock_reset(s);

    atomic_store(&s->running, true);           /* call-backs may arrive at once */
    IOReturn r = (*s->chan)->AllocateChannel(s->chan);
    if (r != kIOReturnSuccess) {
        atomic_store(&s->running, false);
        lg(s, "AllocateChannel failed: 0x%08x", r);
        quiesce(s);
        return -1;
    }
    r = (*s->chan)->Start(s->chan);
    if (r != kIOReturnSuccess) {
        atomic_store(&s->running, false);
        lg(s, "channel Start failed: 0x%08x", r);
        (*s->chan)->ReleaseChannel(s->chan);
        quiesce(s);
        return -1;
    }
    lg(s, "streaming");
    return 0;
}

void slfw_stop(slfw *s)
{
    if (!s || !s->chan) return;
    bool was = atomic_exchange(&s->running, false);
    if (was || s->enabled) {
        (*s->chan)->Stop(s->chan);
        (*s->chan)->ReleaseChannel(s->chan);
    }
    quiesce(s);
    if (was) lg(s, "stopped");
}

void slfw_close(slfw *s)
{
    if (!s) return;
    slfw_stop(s);
    if (s->chan)   (*s->chan)->Release(s->chan);
    if (s->remote) (*s->remote)->Release(s->remote);
    if (s->local)  (*s->local)->Release(s->local);
    if (s->pool)   (*s->pool)->Release(s->pool);
    /* Releasing a port or channel that has streamed completes asynchronously:
     * the kernel's completion comes back through the call-back dispatchers on
     * the run loop. Removing the dispatchers first strands it and the
     * IOFireWireUserClient stays referenced -- one leaked per recording, which
     * can later hold up termination on unplug. Bisected 2026-09-12: 1 user
     * client left without this, 0 with a drain that ends after 20 ms of quiet
     * (~30 ms in practice). Only the run loop's own thread can drive it. */
    if (s->rl && CFRunLoopGetCurrent() == s->rl) {
        for (int k = 0; k < 25; k++)
            if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true) != kCFRunLoopRunHandledSource) break;
    } else if (s->rl) {
        lg(s, "slfw_close called off its run-loop thread: isoch teardown cannot be drained");
    }
    if (s->dev) {
        if (s->dispatched) {
            (*s->dev)->RemoveIsochCallbackDispatcherFromRunLoop(s->dev);
            (*s->dev)->RemoveCallbackDispatcherFromRunLoop(s->dev);
        }
        if (s->opened) (*s->dev)->Close(s->dev);
        (*s->dev)->Release(s->dev);
    }
    if (s->plug)   IODestroyPlugInInterface(s->plug);
    free(s->ring);
    free(s->pktbuf);
    if (g_self == s) g_self = NULL;
    free(s);
}
