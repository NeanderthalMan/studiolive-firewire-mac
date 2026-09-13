// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fwprobe — the one-day spike that proves or kills the adapter.
 *
 * Four questions, in the order that makes a failure cheap:
 *
 *   1. Can a plain user-space process open the console over IOFireWireLib?
 *      No kext, no Reduced Security mode, no restricted entitlement.
 *   2. Do async quadlet reads work? Checked against the Configuration ROM,
 *      whose contents system_profiler already tells us, so a wrong read is
 *      obvious rather than plausible.
 *   3. Is the console actually TCAT DICE? The whole plan assumes it, on the
 *      strength of the chipset family. This reads the DICE private space and
 *      finds out.
 *   4. Does the isochronous API still work on Apple Silicon? This is the part
 *      nobody has exercised in years, and the part with no workaround.
 *
 * Build:  cc -o fwprobe fwprobe.c -framework CoreFoundation -framework IOKit
 * Run:    ./fwprobe              # first FireWire device on the bus
 *         ./fwprobe -g <guid-hex>
 *
 * The DICE register map is from the Linux in-kernel snd-dice driver
 * (sound/firewire/dice/dice-interface.h). It is read here, not trusted: every
 * value is printed with a plausibility note so a wrong map shows up as
 * nonsense rather than as a confident wrong answer.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <IOKit/firewire/IOFireWireLibIsoch.h>
#include <libkern/OSByteOrder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- addresses */

#define CSR_ROM_BASE        0xfffff0000400ULL   /* Configuration ROM         */
#define DICE_PRIVATE_SPACE  0xffffe0000000ULL   /* the DICE offset table     */

/* Offsets into the DICE offset table, in bytes. The VALUES there are in
 * quadlets, relative to DICE_PRIVATE_SPACE — multiply by 4. */
enum { DICE_GLOBAL_OFF = 0x00, DICE_GLOBAL_SZ = 0x04,
       DICE_TX_OFF     = 0x08, DICE_TX_SZ     = 0x0c,
       DICE_RX_OFF     = 0x10, DICE_RX_SZ     = 0x14,
       DICE_EXT_OFF    = 0x18, DICE_EXT_SZ    = 0x1c };

/* Offsets within the DICE global space, in bytes. */
enum { G_OWNER = 0x00, G_NOTIFICATION = 0x08, G_NICK_NAME = 0x0c,
       G_CLOCK_SELECT = 0x4c, G_ENABLE = 0x50, G_STATUS = 0x54,
       G_EXT_STATUS = 0x58, G_SAMPLE_RATE = 0x5c, G_VERSION = 0x60,
       G_CLOCK_CAPS = 0x64 };

/* Within a tx stream block. */
enum { TX_ISOC = 0x00, TX_N_AUDIO = 0x04, TX_N_MIDI = 0x08, TX_SPEED = 0x0c };
/* Within an rx stream block. */
enum { RX_ISOC = 0x00, RX_SEQ_START = 0x04, RX_N_AUDIO = 0x08, RX_N_MIDI = 0x0c };

static const int dice_rates[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000 };

/* ------------------------------------------------------------- byte ordering
 *
 * FireWire is big-endian on the wire and IOFireWireLib does not swap for you.
 * Rather than assume, quadlet 1 of the ROM is the ASCII "1394" magic, so the
 * orientation is measured once and applied everywhere. If neither reading
 * gives "1394" the reads are wrong and everything after it is noise. */
static int g_swap = -1;
static UInt32 host(UInt32 v) { return g_swap ? OSSwapInt32(v) : v; }

/* A name for the handful of IOReturn codes this actually meets, because
 * "0xe00002be" in a spike's output is a second lookup for the reader. */
static const char *ioname(IOReturn r)
{
    switch (r) {
    case kIOReturnSuccess:      return "kIOReturnSuccess";
    case kIOReturnNoResources:  return "kIOReturnNoResources";
    case kIOReturnNoMemory:     return "kIOReturnNoMemory";
    case kIOReturnNotOpen:      return "kIOReturnNotOpen";
    case kIOReturnExclusiveAccess: return "kIOReturnExclusiveAccess";
    case kIOReturnUnsupported:  return "kIOReturnUnsupported";
    case kIOReturnTimeout:      return "kIOReturnTimeout";
    case kIOReturnBusy:         return "kIOReturnBusy";
    default:                    return "?";
    }
}

/* ------------------------------------------------------------------- reading */

static IOFireWireLibDeviceRef g_dev;
static io_object_t            g_svc;

static IOReturn rq(UInt64 a, UInt32 *out)
{
    FWAddress addr;
    addr.nodeID    = 0;                      /* device-relative, 48-bit */
    addr.addressHi = (UInt16)(a >> 32);
    addr.addressLo = (UInt32)(a & 0xffffffffULL);
    return (*g_dev)->ReadQuadlet(g_dev, g_svc, &addr, out, false, 0);
}

/* A quadlet read straight into host order, with the error reported once. */
static int rq_h(UInt64 a, UInt32 *out, const char *what)
{
    UInt32 raw;
    IOReturn r = rq(a, &raw);
    if (r != kIOReturnSuccess) {
        printf("    ! read of %s at 0x%012llx failed: 0x%08x\n", what, a, r);
        return 0;
    }
    *out = host(raw);
    return 1;
}

/* DICE stores strings as big-endian quadlets of little-endian text, so the
 * bytes come out transposed in pairs unless swapped back. Both are printed;
 * one of them will be words. */
static void print_string_block(UInt64 base, int bytes, const char *label)
{
    char raw[260], swapped[260];
    int n = bytes > 256 ? 256 : bytes, i;
    memset(raw, 0, sizeof raw); memset(swapped, 0, sizeof swapped);
    for (i = 0; i < n / 4; i++) {
        UInt32 q;
        if (!rq_h(base + (UInt64)i * 4, &q, label)) return;
        raw[i*4+0] = (q >> 24) & 0xff; raw[i*4+1] = (q >> 16) & 0xff;
        raw[i*4+2] = (q >>  8) & 0xff; raw[i*4+3] = (q      ) & 0xff;
        swapped[i*4+0] = (q      ) & 0xff; swapped[i*4+1] = (q >>  8) & 0xff;
        swapped[i*4+2] = (q >> 16) & 0xff; swapped[i*4+3] = (q >> 24) & 0xff;
    }
    for (i = 0; i < n; i++) {
        if (raw[i] && (raw[i] < 32 || raw[i] > 126)) raw[i] = '.';
        if (swapped[i] && (swapped[i] < 32 || swapped[i] > 126)) swapped[i] = '.';
    }
    printf("    %-18s \"%s\"  /  byte-swapped: \"%s\"\n", label, raw, swapped);
}

/* ------------------------------------------------------- finding the device */

static io_service_t find_device(UInt64 want_guid, UInt64 *got_guid)
{
    io_iterator_t it;
    io_service_t  s, chosen = 0;
    CFMutableDictionaryRef m = IOServiceMatching("IOFireWireDevice");

    if (IOServiceGetMatchingServices(kIOMainPortDefault, m, &it) != kIOReturnSuccess)
        return 0;

    printf("FireWire devices on the bus:\n");
    while ((s = IOIteratorNext(it))) {
        UInt64 guid = 0;
        CFTypeRef g = IORegistryEntryCreateCFProperty(s, CFSTR("GUID"),
                                                      kCFAllocatorDefault, 0);
        if (g) {
            if (CFGetTypeID(g) == CFNumberGetTypeID())
                CFNumberGetValue(g, kCFNumberSInt64Type, &guid);
            CFRelease(g);
        }
        printf("  GUID 0x%012llx", guid);
        CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("FireWire Vendor Name"),
                                                      kCFAllocatorDefault, 0);
        CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR("FireWire Product Name"),
                                                      kCFAllocatorDefault, 0);
        if (v) { char b[128]; if (CFStringGetCString(v, b, sizeof b, kCFStringEncodingUTF8)) printf("  vendor \"%s\"", b); CFRelease(v); }
        if (p) { char b[128]; if (CFStringGetCString(p, b, sizeof b, kCFStringEncodingUTF8)) printf("  product \"%s\"", b); CFRelease(p); }
        printf("\n");

        if (!chosen && (want_guid == 0 || guid == want_guid)) {
            chosen = s; *got_guid = guid;
        } else {
            IOObjectRelease(s);
        }
    }
    IOObjectRelease(it);
    return chosen;
}

/* ------------------------------------------------------------------ sections */

static void probe_config_rom(void)
{
    UInt32 magic_raw, q;
    int i;

    printf("\n[2] Configuration ROM\n");

    /* Quadlet 1 of the ROM is "1394" in ASCII. That fixes the byte order. */
    if (rq(CSR_ROM_BASE + 4, &magic_raw) != kIOReturnSuccess) {
        printf("    ! could not read the ROM at all — async reads are not working.\n");
        return;
    }
    if (magic_raw == 0x31333934)                 { g_swap = 0; }
    else if (OSSwapInt32(magic_raw) == 0x31333934) { g_swap = 1; }
    else {
        printf("    ! quadlet 1 is 0x%08x, which is \"1394\" in neither byte order.\n", magic_raw);
        printf("      Reads are landing somewhere unexpected. Everything below is noise.\n");
        g_swap = 0;
        return;
    }
    printf("    bus name magic OK — byte order established: %s\n",
           g_swap ? "bus is big-endian, swapping to host" : "no swap needed");

    printf("    first 16 quadlets:\n");
    for (i = 0; i < 16; i++) {
        char a[5];
        if (!rq_h(CSR_ROM_BASE + (UInt64)i * 4, &q, "ROM")) return;
        a[0] = (q>>24)&0xff; a[1] = (q>>16)&0xff; a[2] = (q>>8)&0xff; a[3] = q&0xff; a[4] = 0;
        for (int k = 0; k < 4; k++) if (a[k] < 32 || a[k] > 126) a[k] = '.';
        printf("      +0x%02x  0x%08x  |%s|\n", i * 4, q, a);
    }
    /* Quadlets 3 and 4 of the bus info block are node_vendor_id/chip_id. */
    if (rq_h(CSR_ROM_BASE + 0x0c, &q, "vendor/chip hi"))
        printf("    node vendor OUI: 0x%06x   (0x000a92 is PreSonus)\n", q >> 8);
}

static void probe_dice(void)
{
    UInt32 goff, gsz, toff, tsz, roff, rsz, q;
    UInt64 gbase, tbase, rbase;

    printf("\n[3] DICE private space at 0x%012llx\n", DICE_PRIVATE_SPACE);

    if (!rq_h(DICE_PRIVATE_SPACE + DICE_GLOBAL_OFF, &goff, "global_offset")) return;
    if (!rq_h(DICE_PRIVATE_SPACE + DICE_GLOBAL_SZ,  &gsz,  "global_size"))   return;
    if (!rq_h(DICE_PRIVATE_SPACE + DICE_TX_OFF,     &toff, "tx_offset"))     return;
    if (!rq_h(DICE_PRIVATE_SPACE + DICE_TX_SZ,      &tsz,  "tx_size"))       return;
    if (!rq_h(DICE_PRIVATE_SPACE + DICE_RX_OFF,     &roff, "rx_offset"))     return;
    if (!rq_h(DICE_PRIVATE_SPACE + DICE_RX_SZ,      &rsz,  "rx_size"))       return;

    printf("    offset table (values in quadlets):\n");
    printf("      global  off %-6u size %-6u\n", goff, gsz);
    printf("      tx      off %-6u size %-6u\n", toff, tsz);
    printf("      rx      off %-6u size %-6u\n", roff, rsz);

    /* The plausibility gate. A non-DICE device answers this space with
     * 0xffffffff, or with a bus error, or with garbage. Real offsets are
     * small, ascending, and non-zero. */
    if (goff == 0 || goff > 0x10000 || gsz == 0 || gsz > 0x10000 ||
        toff <= goff || roff <= toff) {
        printf("    ! that is not a plausible DICE offset table.\n");
        printf("      The DICE assumption is WRONG, or this space is elsewhere.\n");
        printf("      Stop here and read sound/firewire/dice/ before going further.\n");
        return;
    }
    printf("    plausible. Proceeding on the assumption this really is DICE.\n");

    gbase = DICE_PRIVATE_SPACE + (UInt64)goff * 4;
    tbase = DICE_PRIVATE_SPACE + (UInt64)toff * 4;
    rbase = DICE_PRIVATE_SPACE + (UInt64)roff * 4;

    printf("\n    global space at 0x%012llx:\n", gbase);
    print_string_block(gbase + G_NICK_NAME, 64, "nickname");
    if (rq_h(gbase + G_CLOCK_SELECT, &q, "clock_select")) {
        unsigned src = q & 0xff, rate = (q >> 8) & 0xff;
        printf("    %-18s 0x%08x  source %u, rate index %u", "clock_select", q, src, rate);
        if (rate < sizeof dice_rates / sizeof dice_rates[0])
            printf(" = %d Hz", dice_rates[rate]);
        printf("\n");
    }
    if (rq_h(gbase + G_ENABLE, &q, "enable"))
        printf("    %-18s 0x%08x  (streaming %s)\n", "enable", q, (q & 1) ? "ON" : "off");
    if (rq_h(gbase + G_STATUS, &q, "status")) {
        unsigned locked = q & 1, rate = (q >> 8) & 0xff;
        printf("    %-18s 0x%08x  clock %s", "status", q, locked ? "LOCKED" : "unlocked");
        if (rate < sizeof dice_rates / sizeof dice_rates[0])
            printf(", nominal %d Hz", dice_rates[rate]);
        printf("\n");
    }
    if (rq_h(gbase + G_VERSION, &q, "version"))
        printf("    %-18s 0x%08x  (%u.%u.%u.%u)\n", "version", q,
               (q >> 24) & 0xff, (q >> 16) & 0xff, (q >> 8) & 0xff, q & 0xff);
    if (rq_h(gbase + G_CLOCK_CAPS, &q, "clock_caps"))
        printf("    %-18s 0x%08x\n", "clock_caps", q);

    /* The numbers that decide whether 16 channels is even on offer. */
    printf("\n    tx section at 0x%012llx  (device -> Mac: what we want to capture)\n", tbase);
    UInt32 n_tx = 0, tx_blk = 0;
    if (rq_h(tbase + 0x00, &n_tx,   "tx count") &&
        rq_h(tbase + 0x04, &tx_blk, "tx block size")) {
        printf("    %u tx stream(s), %u quadlets per block\n", n_tx, tx_blk);
        for (UInt32 i = 0; i < n_tx && i < 4; i++) {
            UInt64 b = tbase + 0x08 + (UInt64)i * tx_blk * 4;
            UInt32 iso = 0, na = 0, nm = 0, sp = 0;
            rq_h(b + TX_ISOC,    &iso, "tx isoc");
            rq_h(b + TX_N_AUDIO, &na,  "tx audio");
            rq_h(b + TX_N_MIDI,  &nm,  "tx midi");
            rq_h(b + TX_SPEED,   &sp,  "tx speed");
            printf("      tx[%u]  iso channel %d  audio channels %u  midi ports %u  speed %u\n",
                   i, (int)(SInt32)iso, na, nm, sp);
        }
    }
    printf("\n    rx section at 0x%012llx  (Mac -> device: not needed for capture)\n", rbase);
    UInt32 n_rx = 0, rx_blk = 0;
    if (rq_h(rbase + 0x00, &n_rx,   "rx count") &&
        rq_h(rbase + 0x04, &rx_blk, "rx block size")) {
        printf("    %u rx stream(s), %u quadlets per block\n", n_rx, rx_blk);
        for (UInt32 i = 0; i < n_rx && i < 4; i++) {
            UInt64 b = rbase + 0x08 + (UInt64)i * rx_blk * 4;
            UInt32 iso = 0, na = 0, nm = 0;
            rq_h(b + RX_ISOC,    &iso, "rx isoc");
            rq_h(b + RX_N_AUDIO, &na,  "rx audio");
            rq_h(b + RX_N_MIDI,  &nm,  "rx midi");
            printf("      rx[%u]  iso channel %d  audio channels %u  midi ports %u\n",
                   i, (int)(SInt32)iso, na, nm);
        }
    }
}

/* The question with no workaround. If the isochronous API is gone or broken on
 * Apple Silicon, the whole adapter idea dies here, and it is worth finding out
 * on day one rather than after the DICE protocol is implemented.
 *
 * This deliberately stops short of a DCL program. It asks whether the objects
 * can be created and whether the isochronous resource manager will actually
 * allocate a channel and bandwidth — which is the part that touches hardware. */
static void probe_isoch(void)
{
    printf("\n[4] Isochronous API\n");

    IOFireWireLibIsochChannelRef ch = (*g_dev)->CreateIsochChannel(
        g_dev, true /* doIrm */, 512 /* packetSize */, kFWSpeed400MBit,
        CFUUIDGetUUIDBytes(kIOFireWireIsochChannelInterfaceID));
    if (!ch) {
        printf("    ! CreateIsochChannel returned NULL.\n");
        printf("      No isochronous channel object means no audio, ever, by this route.\n");
        return;
    }
    printf("    CreateIsochChannel: OK\n");

    IOFireWireLibRemoteIsochPortRef talker = (*g_dev)->CreateRemoteIsochPort(
        g_dev, true /* inTalking — the console is the talker */,
        CFUUIDGetUUIDBytes(kIOFireWireRemoteIsochPortInterfaceID));
    if (!talker) {
        printf("    ! CreateRemoteIsochPort returned NULL.\n");
    } else {
        printf("    CreateRemoteIsochPort: OK\n");
        IOReturn r = (*ch)->SetTalker(ch, (IOFireWireLibIsochPortRef)talker);
        printf("    SetTalker: %s (0x%08x)\n", r == kIOReturnSuccess ? "OK" : "failed", r);
    }

    /* Talks to the IRM on the bus. Real hardware, real allocation. A failure
     * here with no listener attached is expected-ish; the IOReturn is the
     * interesting part, so it is printed rather than judged. */
    IOReturn r = (*ch)->AllocateChannel(ch);
    printf("    AllocateChannel: 0x%08x %s %s\n", r, ioname(r),
           r == kIOReturnSuccess ? "— channel and bandwidth reserved on the bus" :
                                   "(expected with no local listener attached; not conclusive)");
    if (r == kIOReturnSuccess) (*ch)->ReleaseChannel(ch);

    if (talker) (*talker)->Release(talker);
    (*ch)->Release(ch);

    printf("\n    Note: a full packet count needs a local isoch port driven by a DCL\n");
    printf("    program, which is the next piece of work and not part of this spike.\n");
}

/* ---------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    UInt64 want = 0, guid = 0;
    if (argc >= 3 && strcmp(argv[1], "-g") == 0) want = strtoull(argv[2], NULL, 16);

    io_service_t svc = find_device(want, &guid);
    if (!svc) {
        fprintf(stderr, "\nNo matching IOFireWireDevice. Is the console powered and connected?\n");
        return 1;
    }
    printf("\nTarget: GUID 0x%012llx\n", guid);

    printf("\n[1] Opening through IOFireWireLib\n");
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    kern_return_t k = IOCreatePlugInInterfaceForService(
        svc, kIOFireWireLibTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    if (k != kIOReturnSuccess || !plug) {
        fprintf(stderr, "    ! IOCreatePlugInInterfaceForService failed: 0x%08x\n", k);
        return 1;
    }
    printf("    plug-in interface created\n");

    /* Newest first. An older device interface still has everything this needs. */
    const CFUUIDRef ids[] = { kIOFireWireDeviceInterfaceID_v9,
                              kIOFireWireDeviceInterfaceID_v8,
                              kIOFireWireDeviceInterfaceID_v7,
                              kIOFireWireDeviceInterfaceID_v6 };
    const char *names[] = { "v9", "v8", "v7", "v6" };
    for (int i = 0; i < 4 && !g_dev; i++) {
        if ((*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(ids[i]),
                                    (void **)&g_dev) == S_OK && g_dev)
            printf("    device interface: %s\n", names[i]);
    }
    if (!g_dev) {
        fprintf(stderr, "    ! no usable device interface\n");
        return 1;
    }

    IOReturn r = (*g_dev)->Open(g_dev);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "    ! Open failed: 0x%08x  (something else may hold the device)\n", r);
        return 1;
    }
    printf("    Open: OK — user space owns this device, with no kext of ours\n");
    g_svc = (*g_dev)->GetDevice(g_dev);

    UInt32 gen = 0; UInt16 node = 0;
    if ((*g_dev)->GetGenerationAndNodeID(g_dev, &gen, &node) == kIOReturnSuccess)
        printf("    bus generation %u, node ID 0x%04x\n", gen, node);

    probe_config_rom();
    if (g_swap >= 0) probe_dice();
    probe_isoch();

    (*g_dev)->Close(g_dev);
    (*g_dev)->Release(g_dev);
    IODestroyPlugInInterface(plug);
    IOObjectRelease(svc);
    printf("\nDone.\n");
    return 0;
}
