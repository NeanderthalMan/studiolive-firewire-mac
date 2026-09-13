// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * isoprobe — the second spike, the last go/no-go before the adapter is real work.
 *
 * fwprobe proved everything except the one piece with no
 * workaround: whether a LOCAL isochronous port, driven by a DCL program, can
 * actually receive packets from the console on Apple Silicon. This answers that
 * and nothing else.
 *
 * The shape of it:
 *
 *   local port  (listener)   a DCL program: a ring of ReceivePacketStart
 *                            commands with a CallProc every GROUP packets,
 *                            jumping back to the top forever
 *   remote port (talker)     the console. Its AllocatePort handler is where we
 *                            write the isoch channel number the IRM picked into
 *                            the console's tx[0] register, and its Start
 *                            handler is where GLOBAL_ENABLE goes high
 *   channel                  binds the two and does the IRM allocation
 *
 * Success is a packet rate near 8000/second: IEC 61883-6 sends one packet per
 * isochronous cycle, and there are 8000 cycles per second at any sample rate.
 * Near 8000 is right. Near zero is not.
 *
 * This does NOT depacketise audio. Turning CIP payloads into channels is the
 * next piece of work and is pointless until the transport is proven.
 *
 * SAFETY: this is the first code in the project that WRITES to the console.
 * GLOBAL_ENABLE is cleared and the tx channel released on every exit path,
 * including signals and abnormal exits — a mixer left streaming into a process
 * that has gone away needs a power cycle to clear.
 *
 * Build: cc -o isoprobe isoprobe.c -framework CoreFoundation -framework IOKit
 * Run:   ./isoprobe [-g <guid-hex>] [-s <seconds>]
 *
 * Deliberately self-contained rather than sharing code with fwprobe.c: that
 * file is the recorded artifact of the first spike and is not worth destabilising to
 * save a hundred lines in a second spike. The real adapter gets a proper
 * structure; two spikes do not.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <libkern/OSByteOrder.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CSR_ROM_BASE        0xfffff0000400ULL
#define DICE_PRIVATE_SPACE  0xffffe0000000ULL

enum { G_OWNER = 0x00, G_CLOCK_SELECT = 0x4c, G_ENABLE = 0x50, G_STATUS = 0x54,
       G_SAMPLE_RATE = 0x5c };
enum { TX_ISOC = 0x00, TX_N_AUDIO = 0x04, TX_N_MIDI = 0x08, TX_SPEED = 0x0c };

static const int dice_rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000 };

/* The DCL ring. 512 packets of 2048 bytes is 1 MB — generous on purpose, since
 * a ring that wraps before the CallProc runs would under-count and read as a
 * transport problem. */
#define NPKT   512
#define PKTSZ  2048
#define GROUP  64

static IOFireWireLibDeviceRef g_dev;
static io_object_t            g_svc;
static int                    g_swap = 0;
static UInt64                 g_gbase, g_tbase;   /* DICE global / tx bases */
static UInt32                 g_chan = ~0u;       /* channel the IRM gave us */
static volatile int           g_enabled = 0;      /* is the console streaming? */
static volatile UInt64        g_packets = 0;
static volatile UInt64        g_callbacks = 0;
static UInt8                 *g_buf;

static UInt32 host(UInt32 v) { return g_swap ? OSSwapInt32(v) : v; }
static UInt32 bus (UInt32 v) { return g_swap ? OSSwapInt32(v) : v; }

static const char *ioname(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess:         return "kIOReturnSuccess";
    case kIOReturnNoResources:     return "kIOReturnNoResources";
    case kIOReturnNoMemory:        return "kIOReturnNoMemory";
    case kIOReturnNotOpen:         return "kIOReturnNotOpen";
    case kIOReturnExclusiveAccess: return "kIOReturnExclusiveAccess";
    case kIOReturnUnsupported:     return "kIOReturnUnsupported";
    case kIOReturnTimeout:         return "kIOReturnTimeout";
    case kIOReturnBusy:            return "kIOReturnBusy";
    case kIOReturnBadArgument:     return "kIOReturnBadArgument";
    case kIOReturnNotPermitted:    return "kIOReturnNotPermitted";
    default:                       return "?";
    }
}

/* ------------------------------------------------------------ device access */

static void fwaddr(UInt64 a, FWAddress *out)
{
    out->nodeID    = 0;
    out->addressHi = (UInt16)(a >> 32);
    out->addressLo = (UInt32)(a & 0xffffffffULL);
}

static int rd(UInt64 a, UInt32 *out, const char *what)
{
    FWAddress addr; UInt32 raw;
    fwaddr(a, &addr);
    IOReturn r = (*g_dev)->ReadQuadlet(g_dev, g_svc, &addr, &raw, false, 0);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "    ! read %s @0x%012llx: 0x%08x %s\n", what, a, r, ioname(r));
        return 0;
    }
    *out = host(raw);
    return 1;
}

static int wr(UInt64 a, UInt32 val, const char *what)
{
    FWAddress addr; UInt32 v = bus(val);
    fwaddr(a, &addr);
    IOReturn r = (*g_dev)->WriteQuadlet(g_dev, g_svc, &addr, v, false, 0);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "    ! write %s=0x%08x @0x%012llx: 0x%08x %s\n",
                what, val, a, r, ioname(r));
        return 0;
    }
    printf("    wrote %-12s = 0x%08x\n", what, val);
    return 1;
}

/* --------------------------------------------------------------- the safety net
 *
 * Called from atexit and from the signal handler. Must be safe to run twice,
 * and safe to run when setup never got far enough to enable anything. */
static void quiesce(void)
{
    if (!g_dev) return;
    if (g_enabled) {
        g_enabled = 0;
        FWAddress a; UInt32 v = bus(0);
        fwaddr(g_gbase + G_ENABLE, &a);
        (*g_dev)->WriteQuadlet(g_dev, g_svc, &a, v, false, 0);
        fprintf(stderr, "    [quiesce] GLOBAL_ENABLE cleared\n");
    }
    if (g_chan != ~0u && g_tbase) {
        g_chan = ~0u;
        FWAddress a; UInt32 v = bus(0xffffffff);
        fwaddr(g_tbase + 0x08 + TX_ISOC, &a);
        (*g_dev)->WriteQuadlet(g_dev, g_svc, &a, v, false, 0);
        fprintf(stderr, "    [quiesce] tx[0] isoc channel released\n");
    }
}

static void on_signal(int sig) { (void)sig; quiesce(); _exit(130); }

/* --------------------------------------------------------- the DCL call-back */

static void dcl_proc(DCLCommand *cmd)
{
    (void)cmd;
    g_callbacks++;
    g_packets += GROUP;
}

/* ------------------------------------------------------ remote port handlers
 *
 * The channel object calls these. They are where the console gets programmed:
 * the IRM picks a channel, hands it to AllocatePort, and that is the number the
 * console has to be told to talk on. */

static IOReturn rp_getsupported(IOFireWireLibIsochPortRef p,
                                IOFWSpeed *speed, UInt64 *chans)
{
    (void)p;
    *speed = kFWSpeed400MBit;   /* the console reported speed 2 = S400 */
    *chans = ~(UInt64)0;        /* no preference; let the IRM choose */
    printf("    [remote] GetSupported -> S400, any channel\n");
    return kIOReturnSuccess;
}

static IOReturn rp_allocate(IOFireWireLibIsochPortRef p, IOFWSpeed speed, UInt32 chan)
{
    (void)p;
    printf("    [remote] AllocatePort -> speed %u, channel %u\n", speed, chan);
    g_chan = chan;
    if (!wr(g_tbase + 0x08 + TX_ISOC, chan, "tx[0].isoc")) return kIOReturnError;
    return kIOReturnSuccess;
}

static IOReturn rp_release(IOFireWireLibIsochPortRef p)
{
    (void)p;
    printf("    [remote] ReleasePort\n");
    if (g_chan != ~0u) {
        wr(g_tbase + 0x08 + TX_ISOC, 0xffffffff, "tx[0].isoc");
        g_chan = ~0u;
    }
    return kIOReturnSuccess;
}

static IOReturn rp_start(IOFireWireLibIsochPortRef p)
{
    (void)p;
    printf("    [remote] Start -> setting GLOBAL_ENABLE\n");
    if (!wr(g_gbase + G_ENABLE, 1, "GLOBAL_ENABLE")) return kIOReturnError;
    g_enabled = 1;
    return kIOReturnSuccess;
}

static IOReturn rp_stop(IOFireWireLibIsochPortRef p)
{
    (void)p;
    printf("    [remote] Stop -> clearing GLOBAL_ENABLE\n");
    wr(g_gbase + G_ENABLE, 0, "GLOBAL_ENABLE");
    g_enabled = 0;
    return kIOReturnSuccess;
}

/* ---------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    UInt64 want = 0, guid = 0;
    double secs = 10.0;
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "-g")) want = strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "-s")) secs = atof(argv[++i]);
    }

    /* --- find and open ------------------------------------------------- */
    io_iterator_t it; io_service_t s, svc = 0;
    IOServiceGetMatchingServices(kIOMainPortDefault,
                                 IOServiceMatching("IOFireWireDevice"), &it);
    while ((s = IOIteratorNext(it))) {
        UInt64 g = 0;
        CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR("GUID"), kCFAllocatorDefault, 0);
        if (p) { if (CFGetTypeID(p) == CFNumberGetTypeID())
                     CFNumberGetValue(p, kCFNumberSInt64Type, &g);
                 CFRelease(p); }
        if (!svc && (want == 0 || g == want)) { svc = s; guid = g; }
        else IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (!svc) { fprintf(stderr, "No matching IOFireWireDevice.\n"); return 1; }
    printf("Target: GUID 0x%012llx\n", guid);

    IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
    if (IOCreatePlugInInterfaceForService(svc, kIOFireWireLibTypeID,
            kIOCFPlugInInterfaceID, &plug, &score) != kIOReturnSuccess || !plug) {
        fprintf(stderr, "plug-in creation failed\n"); return 1;
    }
    if ((*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9),
                                (void **)&g_dev) != S_OK || !g_dev) {
        fprintf(stderr, "no v9 device interface\n"); return 1;
    }
    IOReturn r = (*g_dev)->Open(g_dev);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "Open failed: 0x%08x %s\n", r, ioname(r)); return 1;
    }
    g_svc = (*g_dev)->GetDevice(g_dev);
    /* Before any isoch object exists: a local port binds its callback path at
     * creation, and registering the dispatcher afterwards left every CallProc
     * silent on the first run even though DMA filled the whole ring. */
    (*g_dev)->AddCallbackDispatcherToRunLoop(g_dev, CFRunLoopGetCurrent());
    (*g_dev)->AddIsochCallbackDispatcherToRunLoop(g_dev, CFRunLoopGetCurrent());
    atexit(quiesce);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("Opened. IOFireWireLib v9.\n");

    /* --- byte order, off the ROM's own magic --------------------------- */
    FWAddress a; UInt32 magic;
    fwaddr(CSR_ROM_BASE + 4, &a);
    if ((*g_dev)->ReadQuadlet(g_dev, g_svc, &a, &magic, false, 0) != kIOReturnSuccess) {
        fprintf(stderr, "ROM read failed\n"); return 1;
    }
    if (magic == 0x31333934) g_swap = 0;
    else if (OSSwapInt32(magic) == 0x31333934) g_swap = 1;
    else { fprintf(stderr, "ROM magic is 0x%08x — reads are wrong\n", magic); return 1; }

    /* --- DICE layout --------------------------------------------------- */
    UInt32 goff, toff, q, n_audio = 0, n_midi = 0, rate_idx = 0;
    if (!rd(DICE_PRIVATE_SPACE + 0x00, &goff, "global_offset")) return 1;
    if (!rd(DICE_PRIVATE_SPACE + 0x08, &toff, "tx_offset")) return 1;
    g_gbase = DICE_PRIVATE_SPACE + (UInt64)goff * 4;
    g_tbase = DICE_PRIVATE_SPACE + (UInt64)toff * 4;
    printf("DICE global @0x%012llx, tx @0x%012llx\n", g_gbase, g_tbase);

    rd(g_tbase + 0x08 + TX_N_AUDIO, &n_audio, "tx audio");
    rd(g_tbase + 0x08 + TX_N_MIDI,  &n_midi,  "tx midi");
    if (rd(g_gbase + G_STATUS, &q, "status")) {
        rate_idx = (q >> 8) & 0xff;
        printf("clock %s", (q & 1) ? "LOCKED" : "UNLOCKED");
        if (rate_idx < sizeof dice_rates / sizeof dice_rates[0])
            printf(", %d Hz", dice_rates[rate_idx]);
        printf(" — tx offers %u audio + %u midi\n", n_audio, n_midi);
        if (!(q & 1))
            printf("  ! clock is unlocked; a stream may not start at all\n");
    }
    /* Leave clock_select alone. Changing the console's clock is a side effect
     * this test does not need, and the packet rate is 8000/s at every rate. */

    /* --- the DCL program ---------------------------------------------- */
    printf("\nBuilding the DCL program: %d packets x %d bytes, CallProc every %d\n",
           NPKT, PKTSZ, GROUP);
    g_buf = valloc(NPKT * PKTSZ);           /* page-aligned for the DMA */
    if (!g_buf) { fprintf(stderr, "buffer allocation failed\n"); return 1; }
    memset(g_buf, 0, NPKT * PKTSZ);

    IOFireWireLibDCLCommandPoolRef pool = (*g_dev)->CreateDCLCommandPool(
        g_dev, NPKT * 64 + 4096, CFUUIDGetUUIDBytes(kIOFireWireDCLCommandPoolInterfaceID));
    if (!pool) { fprintf(stderr, "CreateDCLCommandPool failed\n"); return 1; }

    DCLCommand *first = (*pool)->AllocateLabelDCL(pool, NULL);
    if (!first) { fprintf(stderr, "AllocateLabelDCL failed\n"); return 1; }
    DCLCommand *dcl = first;
    /* IEC 61883 CIP packets carry tag = 1, and an OHCI isochronous receive
     * context matches on a tag mask. Accept all four tags rather than assume
     * the default includes the one we need — this is the leading suspect for a
     * silent context. */
    dcl = (*pool)->AllocateSetTagSyncBitsDCL(pool, dcl, 0xF, 0x0);
    if (!dcl) { fprintf(stderr, "AllocateSetTagSyncBitsDCL failed\n"); return 1; }
    for (int i = 0; i < NPKT; i++) {
        dcl = (*pool)->AllocateReceivePacketStartDCL(pool, dcl, g_buf + (size_t)i * PKTSZ, PKTSZ);
        if (!dcl) { fprintf(stderr, "AllocateReceivePacketStartDCL failed at %d\n", i); return 1; }
        if ((i + 1) % GROUP == 0) {
            dcl = (*pool)->AllocateCallProcDCL(pool, dcl, dcl_proc, (DCLCallProcDataType)(uintptr_t)i);
            if (!dcl) { fprintf(stderr, "AllocateCallProcDCL failed at %d\n", i); return 1; }
        }
    }
    dcl = (*pool)->AllocateJumpDCL(pool, dcl, (DCLLabel *)first);
    if (!dcl) { fprintf(stderr, "AllocateJumpDCL failed\n"); return 1; }
    printf("    program built\n");

    /* --- ports and channel -------------------------------------------- */
    IOVirtualRange bufRange = { (IOVirtualAddress)g_buf, NPKT * PKTSZ };
    IOFireWireLibLocalIsochPortRef local = (*g_dev)->CreateLocalIsochPort(
        g_dev, false /* listening */, first,
        kFWDCLImmediateEvent, 0, 0,
        NULL, 0, &bufRange, 1,
        CFUUIDGetUUIDBytes(kIOFireWireLocalIsochPortInterfaceID));
    if (!local) {
        fprintf(stderr, "\n! CreateLocalIsochPort returned NULL.\n"
                        "  This is the failure with no workaround: the DCL program was\n"
                        "  rejected or local isoch ports do not work here. STOP.\n");
        return 1;
    }
    printf("    CreateLocalIsochPort: OK\n");

    IOFireWireLibRemoteIsochPortRef remote = (*g_dev)->CreateRemoteIsochPort(
        g_dev, true /* the console talks */,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!remote) { fprintf(stderr, "CreateRemoteIsochPort failed\n"); return 1; }
    (*remote)->SetGetSupportedHandler(remote, rp_getsupported);
    (*remote)->SetAllocatePortHandler (remote, rp_allocate);
    (*remote)->SetReleasePortHandler  (remote, rp_release);
    (*remote)->SetStartHandler        (remote, rp_start);
    (*remote)->SetStopHandler         (remote, rp_stop);

    IOFireWireLibIsochChannelRef ch = (*g_dev)->CreateIsochChannel(
        g_dev, true /* doIrm */, PKTSZ, kFWSpeed400MBit,
        CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!ch) { fprintf(stderr, "CreateIsochChannel failed\n"); return 1; }

    r = (*ch)->SetTalker(ch, (IOFireWireLibIsochPortRef)remote);
    printf("    SetTalker: 0x%08x %s\n", r, ioname(r));
    r = (*ch)->AddListener(ch, (IOFireWireLibIsochPortRef)local);
    printf("    AddListener: 0x%08x %s\n", r, ioname(r));

    (*ch)->TurnOnNotification(ch);

    printf("\nAllocating the channel (this talks to the IRM and programs the console)\n");
    r = (*ch)->AllocateChannel(ch);
    printf("    AllocateChannel: 0x%08x %s\n", r, ioname(r));
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "  ! IRM allocation failed with a real listener attached.\n"
                        "    That is conclusive, unlike fwprobe's talker-only result. STOP.\n");
        return 1;
    }

    printf("\nStarting\n");
    r = (*ch)->Start(ch);
    printf("    Start: 0x%08x %s\n", r, ioname(r));
    if (r != kIOReturnSuccess) { quiesce(); return 1; }

    /* Did any of it stick? A console that silently refused to start looks
     * exactly like a receive context that is not matching, so ask it. */
    printf("\nReadback after Start:\n");
    UInt32 v;
    if (rd(g_gbase + G_ENABLE, &v, "GLOBAL_ENABLE"))
        printf("    GLOBAL_ENABLE  = 0x%08x %s\n", v,
               v ? "(console says it is streaming)" : "(console did NOT stay enabled)");
    if (rd(g_gbase + G_STATUS, &v, "GLOBAL_STATUS")) {
        unsigned ri = (v >> 8) & 0xff;
        printf("    GLOBAL_STATUS  = 0x%08x  clock %s", v, (v & 1) ? "LOCKED" : "unlocked");
        if (ri < sizeof dice_rates / sizeof dice_rates[0]) printf(", %d Hz", dice_rates[ri]);
        printf("\n");
    }
    if (rd(g_tbase + 0x08 + TX_ISOC, &v, "tx[0].isoc"))
        printf("    tx[0].isoc     = 0x%08x %s\n", v,
               v == g_chan ? "(channel stuck)" : "(channel did NOT stick)");
    if (rd(g_tbase + 0x08 + TX_SPEED, &v, "tx[0].speed"))
        printf("    tx[0].speed    = 0x%08x\n", v);

    printf("\nReceiving for %.1f seconds...\n", secs);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs, false);

    (*ch)->Stop(ch);
    UInt64 pkts = g_packets, cbs = g_callbacks;

    /* --- analysis: decode every slot, then decide -------------------- */
    printf("\nCallbacks: %llu (%llu packets counted by CallProc)\n", cbs, pkts);

    /* Layout of each slot, established on the first run:
     *   [0] isochronous packet header, in HOST byte order (OHCI writes it so)
     *   [1] CIP quadlet 1, big-endian: SID | DBS | FN/QPC/SPH | DBC
     *   [2] CIP quadlet 2, big-endian: FMT | FDF | SYT
     *   [3..] payload, DBS quadlets per data block                          */
    int written = 0, data = 0, empty = 0, other = 0;
    int dbc_ok = 0, dbc_bad = 0, have_prev = 0;
    unsigned prev_dbc = 0, prev_blocks = 0, seen_dbs = 0, seen_fmt = 0, seen_fdf = 0;
    unsigned seen_chan = 0xff, seen_tag = 0xff;
    for (int i = 0; i < NPKT; i++) {
        UInt32 *q = (UInt32 *)(g_buf + (size_t)i * PKTSZ);
        if (!q[0] && !q[1] && !q[2]) continue;
        written++;
        UInt32 ih  = q[0];
        UInt32 c1  = OSSwapBigToHostInt32(q[1]);
        UInt32 c2  = OSSwapBigToHostInt32(q[2]);
        unsigned len   = ih >> 16, tag = (ih >> 14) & 3, chan = (ih >> 8) & 0x3f;
        unsigned tcode = (ih >> 4) & 0xf;
        unsigned dbs   = (c1 >> 16) & 0xff, dbc = c1 & 0xff;
        unsigned fmt   = (c2 >> 24) & 0x3f, fdf = (c2 >> 16) & 0xff;
        if (tcode != 0xa || len < 8) { other++; have_prev = 0; continue; }
        seen_chan = chan; seen_tag = tag;
        unsigned blocks = (dbs && len > 8) ? (len - 8) / (dbs * 4) : 0;
        if (blocks) { data++; seen_dbs = dbs; seen_fmt = fmt; seen_fdf = fdf; }
        else          empty++;
        if (have_prev) {
            /* An empty packet carries the DBC of the NEXT data block, so the
             * expected value is simply the previous DBC plus its block count. */
            if (dbc == ((prev_dbc + prev_blocks) & 0xff)) dbc_ok++; else dbc_bad++;
        }
        prev_dbc = dbc; prev_blocks = blocks; have_prev = 1;
    }

    static const char *sfc_names[] = { "32 kHz", "44.1 kHz", "48 kHz", "88.2 kHz",
                                       "96 kHz", "176.4 kHz", "192 kHz" };
    unsigned sfc = seen_fdf & 0x07;
    printf("\nRing analysis (%d slots written of %d):\n", written, NPKT);
    printf("  isoch header   tag %u, channel %u (IRM allocated %u)\n",
           seen_tag, seen_chan, g_chan == ~0u ? 0 : g_chan);
    printf("  packets        %d data, %d empty, %d unrecognised\n", data, empty, other);
    printf("  CIP DBS        %u quadlets per data block (tx[0] reports %u channels)\n",
           seen_dbs, n_audio);
    printf("  CIP FMT        0x%02x %s\n", seen_fmt, seen_fmt == 0x10 ? "= IEC 61883-6 AM824" : "(unexpected)");
    printf("  CIP FDF        0x%02x, SFC %u = %s\n", seen_fdf, sfc, sfc < 7 ? sfc_names[sfc] : "?");
    printf("  DBC continuity %d good steps, %d breaks", dbc_ok, dbc_bad);
    printf("  (the ring wraps, so at most 1 break is expected, where newest meets oldest)\n");

    int transport = (data > 0 && seen_fmt == 0x10 && seen_dbs == n_audio && dbc_bad <= 1);
    double rate = pkts / secs;

    printf("\nVERDICT: ");
    if (transport && cbs > 0 && rate > 6000 && rate < 10000)
        printf("transport PROVEN and callbacks work — %.0f packets/s against ~8000 expected.\n", rate);
    else if (transport && cbs > 0)
        printf("transport PROVEN; callbacks fire but the rate is %.0f/s, not ~8000.\n", rate);
    else if (transport)
        printf("transport PROVEN by the stream itself; CallProc dispatch still broken.\n"
               "         A correctly sequenced AM824 stream at the right channel count is not\n"
               "         something a broken receive path can fake. The dispatch bug is separate.\n");
    else if (written)
        printf("DMA wrote data but it does not decode as the expected stream. Investigate.\n");
    else
        printf("nothing arrived. Console not talking, or the receive context is not matching.\n");

    /* A few payload samples from the first data packet, to see what the label
     * byte actually is before any depacketiser is written around an assumption. */
    for (int i = 0; i < NPKT; i++) {
        UInt32 *q = (UInt32 *)(g_buf + (size_t)i * PKTSZ);
        unsigned len = q[0] >> 16;
        if (((q[0] >> 4) & 0xf) != 0xa || len <= 8) continue;
        printf("\nFirst data packet (slot %d), first data block, first 8 channels:\n  ", i);
        for (int c = 0; c < 8; c++) {
            UInt32 v = OSSwapBigToHostInt32(q[3 + c]);
            printf("ch%d %08x  ", c + 1, v);
            if (c == 3) printf("\n  ");
        }
        printf("\n  (top byte is the AM824 label; the low 24 bits are the sample)\n");
        break;
    }

    (*ch)->ReleaseChannel(ch);
    quiesce();
    (*ch)->Release(ch);
    (*remote)->Release(remote);
    (*local)->Release(local);
    (*pool)->Release(pool);
    (*g_dev)->Close(g_dev);
    printf("\nDone.\n");
    return 0;
}
