// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * StudioLiveFW — a Core Audio device for a StudioLive AI console over FireWire.
 *
 * An AudioServerPlugIn: Core Audio loads it into its sandboxed driver service,
 * and every app — Studio One included — sees an ordinary input device with one
 * named channel per console channel. All the FireWire work is in src/slfw.c;
 * this file is the Core Audio object model and the I/O cycle.
 *
 * Objects: the plug-in, one device (published while a PreSonus console is on
 * the bus), and one input stream carrying every tx[0] channel.
 *
 * Threads:
 *   - a private "FireWire" thread runs a CFRunLoop that owns everything
 *     IOKit: device arrival/removal notifications, opening and starting the
 *     stream, and the DCL call-backs that fill the frame ring;
 *   - Core Audio's own threads call the property functions and StartIO/StopIO,
 *     which hop onto the FireWire thread for anything that touches the device;
 *   - Core Audio's real-time I/O thread calls GetZeroTimeStamp and
 *     DoIOOperation, which only use slfw's lock-free readers.
 *
 * The device is opened lazily: identity is read and the device closed again on
 * arrival, and the stream is only opened while some app is doing I/O. That
 * keeps the console quiet and the command-line tools usable the rest of the
 * time, and costs a few tens of milliseconds at StartIO.
 *
 * Logs go to the unified log, readable only as root (plain `log show` returns
 * nothing), under the process com.apple.audio.Core-Audio-Driver-Service.helper:
 *   sudo log stream --info --predicate 'subsystem == "n12n.presonus-adapter"'
 */
#include "../src/slfw.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

enum { kObj_PlugIn = kAudioObjectPlugInObject, kObj_Device = 2, kObj_Stream = 3 };

/* Frames per zero-timestamp period; must exceed any I/O buffer size. */
#define kZeroTSPeriod   16384
/* Frames arrive in call-back groups of ~256 at 48 kHz; the fitted clock maps a
 * group's last frame to its arrival, so input must be read at least that late,
 * plus call-back jitter. */
#define kSafetyOffset   1024
#define kLatencyFrames  256
#define kMaxIOFrames    16384

static os_log_t                 gLog;
static AudioServerPlugInHostRef gHost;
static _Atomic ULONG            gRefCount = 1;

/* identity — written on the FireWire thread under gLock, read by property calls */
static pthread_mutex_t          gLock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool             gPresent;
static slfw_info                gInfo;

/* I/O */
static _Atomic(slfw *)          gFW;            /* open only while I/O runs */
static _Atomic int              gInIO;          /* I/O-thread calls using gFW right now */
static _Atomic UInt32           gClients;
static _Atomic bool             gRunning;
static int32_t                 *gScratch;
static _Atomic uint64_t         gUnderruns;
static double                   gLastTpf, gLastBaseHost;
static uint64_t                 gLastBaseFrame;
static uint32_t                 gLastSeed;
static _Atomic bool             gHaveClock;
/* The last period boundary handed to Core Audio. Reported timestamps are built
 * forward from this latch rather than recomputed from the fit each call: the
 * fit slides a little every half-second, and recomputing let the same sample
 * time come back with an earlier host time (halcheck caught 4 in 6 s). */
static double                   gZSample = -1, gZHost;
static uint64_t                 gZSeed;

/* FireWire thread */
static CFRunLoopRef             gFWRunLoop;
static IONotificationPortRef    gNotify;
static io_iterator_t            gArrivals, gRemovals;

static void fwlog(const char *m) { os_log(gLog, "%{public}s", m); }

static void notify(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (gHost) gHost->PropertiesChanged(gHost, obj, 1, &a);
}

/* A console read while it is still booting reports a locked 48 kHz before it
 * loads its saved rate, and the rate cached at arrival then stayed wrong while
 * 44.1 kHz frames arrived (2026-09-13). Every open re-reads it; a difference
 * goes through Core Audio's configuration change, which stops I/O, lets
 * PerformDeviceConfigurationChange publish the new rate, and starts I/O again. */
static _Atomic unsigned         gPendingRate;

static unsigned advertised_rate(void)
{
    pthread_mutex_lock(&gLock);
    unsigned r = gInfo.sample_rate;
    pthread_mutex_unlock(&gLock);
    return r;
}

/* The rate a stream's own packets imply. Blocking-mode IEC 61883-6 sends one
 * packet per FireWire cycle, 8000 a second, empty or not, so frames / packets
 * x 8000 is the sample rate: exact, readable within half a second where the
 * clock fit needs several, and right even while GLOBAL_STATUS is wrong outright
 * during boot. 0 when there are too few packets to trust, or the result is not
 * near a standard rate. */
static unsigned cadence_rate(uint64_t frames, uint64_t packets)
{
    static const unsigned standard[] = { 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
    if (packets < 4000) return 0;
    double r = (double)frames / (double)packets * 8000.0;
    unsigned best = 0;
    double off = 1e12;
    for (unsigned i = 0; i < sizeof standard / sizeof standard[0]; i++) {
        double dist = fabs(r - standard[i]);
        if (dist < off) { off = dist; best = standard[i]; }
    }
    return off <= best * 0.02 ? best : 0;
}

/* FireWire thread, straight after an open. The stream runs at the rate Core
 * Audio was already told, whatever GLOBAL_STATUS says at this moment: a booting
 * console flips its status between 48 and 44.1 kHz on every reinitialisation,
 * and publishing each reading bounced Core Audio 48 -> 44.1 -> 48 -> 44.1 within
 * 15 s (2026-09-13 12:13). The stream's own packet cadence, checked by health(),
 * is the only thing allowed to change the advertised rate. */
static unsigned settle_rate(slfw *fw)
{
    unsigned status = slfw_get_info(fw)->sample_rate;
    unsigned adv = advertised_rate();
    if (!adv) return status;
    if (status != adv) {
        os_log(gLog, "console status says %u Hz; the stream stays at the advertised %u Hz until its cadence says otherwise",
               status, adv);
        slfw_override_rate(fw, adv);
    }
    return adv;
}

static void publish_rate(unsigned rate, const char *why)
{
    unsigned old = advertised_rate();
    if (!rate || rate == old) return;
    os_log(gLog, "console is at %u Hz but %u Hz was advertised (%{public}s); requesting a configuration change",
           rate, old, why);
    atomic_store(&gPendingRate, rate);
    if (gHost) gHost->RequestDeviceConfigurationChange(gHost, kObj_Device, 0, NULL);
}

/* Run a block on the FireWire thread and wait for it. */
static bool on_fw(void (^block)(void))
{
    if (CFRunLoopGetCurrent() == gFWRunLoop) { block(); return true; }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CFRunLoopPerformBlock(gFWRunLoop, kCFRunLoopDefaultMode, ^{ block(); dispatch_semaphore_signal(done); });
    CFRunLoopWakeUp(gFWRunLoop);
    return dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
}

/* Tear down the open stream — FireWire thread only. The pointer is cleared
 * first, then any I/O-thread call already holding it is waited out (at most one
 * cycle) before it is freed, so a read can never touch a closed stream. */
static void release_stream(const char *why)
{
    slfw *fw = atomic_exchange(&gFW, NULL);
    if (!fw) return;
    for (int i = 0; i < 500 && atomic_load(&gInIO) > 0; i++) usleep(1000);
    slfw_stop(fw);
    slfw_close(fw);
    os_log(gLog, "stream released: %{public}s", why);
}

/* ------------------------------------------------------------ presence */

static uint64_t guid_of(io_service_t svc)
{
    uint64_t v = 0;
    CFTypeRef p = IORegistryEntryCreateCFProperty(svc, CFSTR("GUID"), kCFAllocatorDefault, 0);
    if (p) { if (CFGetTypeID(p) == CFNumberGetTypeID()) CFNumberGetValue(p, kCFNumberSInt64Type, &v); CFRelease(p); }
    return v;
}

static void arrivals(void *ref, io_iterator_t it)
{
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        if (!atomic_load(&gPresent) && (guid_of(svc) >> 40) == 0x000a92) {
            char err[256];
            slfw *s = slfw_open(svc, gFWRunLoop, fwlog, err, sizeof err);
            if (s) {
                pthread_mutex_lock(&gLock);
                gInfo = *slfw_get_info(s);
                pthread_mutex_unlock(&gLock);
                slfw_close(s);
                atomic_store(&gPresent, true);
                os_log(gLog, "console present: %{public}s, %u channels at %u Hz",
                       gInfo.nickname, gInfo.channels, gInfo.sample_rate);
                notify(kObj_PlugIn, kAudioPlugInPropertyDeviceList);
                notify(kObj_PlugIn, kAudioObjectPropertyOwnedObjects);
            } else {
                os_log_error(gLog, "console found but could not be opened: %{public}s", err);
            }
        }
        IOObjectRelease(svc);
    }
}

static void removals(void *ref, io_iterator_t it)
{
    io_service_t svc;
    while ((svc = IOIteratorNext(it))) {
        if (atomic_load(&gPresent) && guid_of(svc) == gInfo.guid) {
            os_log(gLog, "console removed");
            atomic_store(&gPresent, false);
            atomic_store(&gRunning, false);
            /* Free it here, not in StopIO. Core Audio does not call StopIO for a
             * device it has already dropped, so leaving it to StopIO kept the
             * FireWire user client open: the kernel's termination of the device
             * sat "busy" for minutes, and slfw's one-console guard would have
             * refused the console when it came back. */
            release_stream("console removed");
            atomic_store(&gClients, 0);
            notify(kObj_Device, kAudioDevicePropertyDeviceIsAlive);
            notify(kObj_PlugIn, kAudioPlugInPropertyDeviceList);
            notify(kObj_PlugIn, kAudioObjectPropertyOwnedObjects);
        }
        IOObjectRelease(svc);
    }
}

/* Set whenever a stream is (re)started, so health() measures its cadence from a
 * clean window instead of across two streams' counters. */
static _Atomic bool             gNewStream;

/* FireWire thread. Close the stream and open it again from scratch, which
 * re-programs the console's transmitter. The rate stays what Core Audio was
 * told (settle_rate); health() corrects it from the packets if it is wrong. */
static void reopen_stream(const char *why)
{
    release_stream(why);
    io_service_t svc = slfw_find_service(gInfo.guid);
    if (!svc) { os_log_error(gLog, "reopen (%{public}s): console not on the bus", why); return; }
    char err[256];
    slfw *fw = slfw_open(svc, gFWRunLoop, fwlog, err, sizeof err);
    IOObjectRelease(svc);
    if (!fw) { os_log_error(gLog, "reopen (%{public}s): open failed: %{public}s", why, err); return; }
    if (slfw_start(fw) != 0) { slfw_close(fw); os_log_error(gLog, "reopen (%{public}s): start failed", why); return; }
    settle_rate(fw);
    atomic_store(&gNewStream, true);
    atomic_store(&gFW, fw);
    os_log(gLog, "stream reopened (%{public}s)", why);
}

/* Once a second: recover from a bus reset or a stall, correct a wrong advertised
 * rate, and log trouble where it can be read. */
static void health(CFRunLoopTimerRef t, void *info)
{
    static uint64_t last_breaks, last_lost, last_under, last_frames;
    static uint64_t win_frames, win_packets;
    static bool win_open;
    static int still;
    slfw *fw = atomic_load(&gFW);          /* FireWire thread: nothing else frees it */
    if (!atomic_load(&gRunning) || !atomic_load(&gPresent)) { still = 0; win_open = false; return; }
    if (!fw) {
        /* A reopen failed: keep trying every other second while I/O runs. */
        if (++still >= 2) { still = 0; reopen_stream("retry"); }
        return;
    }
    slfw_stats st;
    slfw_get_stats(fw, &st);
    /* A console that re-initialises under an open stream (it does while booting)
     * stops sending without a DBC break or a force-stop, and nothing else
     * notices: frames froze for 90 s on 2026-09-13 with I/O still running. */
    if (st.frames == last_frames) {
        if (++still >= 2) {
            os_log(gLog, "stream stalled at frame %llu; reopening", st.frames);
            still = 0; last_frames = 0;
            reopen_stream("stalled");
            return;
        }
    } else {
        still = 0;
    }
    last_frames = st.frames;
    static int readings;
    if (st.force_stopped) {
        os_log(gLog, "stream force-stopped; restarting");
        slfw_stop(fw);
        if (slfw_start(fw) != 0) os_log_error(gLog, "restart failed");
        /* Restarted in place: the frame count starts over, the packet count
         * does not. */
        slfw_get_stats(fw, &st);
        win_frames = 0;
        win_packets = st.packets;
        win_open = true;
        readings = 0;
    }

    /* The stream's packet cadence is the only authority on the rate.
     * Checked every second over a window of up to two seconds. A fresh
     * stream's counters start at zero, so it is measured from its first packet:
     * boot streams live 2-5 s, and a window opened on the first tick and read on
     * the second never saw one (2026-09-13 12:13). */
    if (atomic_exchange(&gNewStream, false) || !win_open) {
        win_frames = 0;
        win_packets = 0;
        win_open = true;
        readings = 0;
    }
    if (st.frames >= win_frames && st.packets >= win_packets) {
        uint64_t df = st.frames - win_frames, dp = st.packets - win_packets;
        unsigned measured = cadence_rate(df, dp);
        unsigned advertised = advertised_rate();
        if (readings < 3 && dp) {
            readings++;
            os_log(gLog, "cadence reading %d: %llu frames / %llu packets = %.0f Hz -> %u (advertised %u)",
                   readings, df, dp, (double)df / (double)dp * 8000.0, measured, advertised);
        }
        if (measured && measured != advertised) {
            os_log(gLog, "stream cadence says %u Hz but %u Hz is advertised; correcting now",
                   measured, advertised);
            slfw_override_rate(fw, measured);
            publish_rate(measured, "measured from packet cadence");
            win_open = false;              /* the configuration change reopens the stream */
            return;
        }
        if (dp >= 16000) {
            win_frames = st.frames;
            win_packets = st.packets;
        }
    } else {
        win_frames = st.frames;            /* counters went backwards: start over */
        win_packets = st.packets;
    }
    uint64_t under = atomic_load(&gUnderruns);
    if (st.dbc_breaks != last_breaks || st.frames_lost != last_lost || under != last_under) {
        os_log(gLog, "stats: frames %llu, DBC breaks %llu, frames lost %llu, read underruns %llu",
               st.frames, st.dbc_breaks, st.frames_lost, under);
        last_breaks = st.dbc_breaks; last_lost = st.frames_lost; last_under = under;
    }
}

static void *fw_thread(void *arg)
{
    dispatch_semaphore_t ready = arg;
    pthread_setname_np("StudioLiveFW FireWire");
    gFWRunLoop = (CFRunLoopRef)CFRetain(CFRunLoopGetCurrent());

    gNotify = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(gFWRunLoop, IONotificationPortGetRunLoopSource(gNotify), kCFRunLoopDefaultMode);
    IOServiceAddMatchingNotification(gNotify, kIOFirstMatchNotification,
                                     IOServiceMatching("IOFireWireDevice"), arrivals, NULL, &gArrivals);
    IOServiceAddMatchingNotification(gNotify, kIOTerminatedNotification,
                                     IOServiceMatching("IOFireWireDevice"), removals, NULL, &gRemovals);
    arrivals(NULL, gArrivals);                 /* arms the iterators */
    removals(NULL, gRemovals);

    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1, 1, 0, 0, health, NULL);
    CFRunLoopAddTimer(gFWRunLoop, t, kCFRunLoopDefaultMode);

    dispatch_semaphore_signal(ready);
    CFRunLoopRun();
    return NULL;
}

/* ----------------------------------------------------------- properties */

static CFStringRef cfstr(const char *fmt, ...)
{
    char b[256];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    return CFStringCreateWithCString(NULL, b, kCFStringEncodingUTF8);
}

static AudioStreamBasicDescription stream_format(void)
{
    UInt32 ch = gInfo.channels;
    AudioStreamBasicDescription f = {
        .mSampleRate = gInfo.sample_rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4 * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 4 * ch,
        .mChannelsPerFrame = ch, .mBitsPerChannel = 32,
    };
    return f;
}

/* One function answers "does it exist", "how big" and "what is it": with out
 * NULL it only sizes. Unknown combinations return UnknownPropertyError, which
 * is also how HasProperty says no. */
static OSStatus prop(AudioObjectID obj, const AudioObjectPropertyAddress *a,
                     UInt32 qsz, const void *q, UInt32 insz, UInt32 *outsz, void *out)
{
#define SIZE(n)   do { *outsz = (UInt32)(n); if (!out) return noErr; \
                       if (insz < *outsz) return kAudioHardwareBadPropertySizeError; } while (0)
#define PUT(T, v) do { SIZE(sizeof(T)); *(T *)out = (v); return noErr; } while (0)
#define PUTCF(v)  do { SIZE(sizeof(CFStringRef)); *(CFStringRef *)out = (v); return noErr; } while (0)
#define PUTIDS(n, id) do { UInt32 _n = (n); *outsz = _n * sizeof(AudioObjectID); if (!out) return noErr; \
                       if (insz < *outsz) *outsz = insz / sizeof(AudioObjectID) * sizeof(AudioObjectID); \
                       if (*outsz) *(AudioObjectID *)out = (id); return noErr; } while (0)

    AudioObjectPropertySelector sel = a->mSelector;
    AudioObjectPropertyScope scope = a->mScope;
    bool present = atomic_load(&gPresent);
    UInt32 ch = gInfo.channels;

    if (obj == kObj_PlugIn) {
        switch (sel) {
        case kAudioObjectPropertyBaseClass:       PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:           PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner:           PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer:    PUTCF(CFSTR("presonus-adapter"));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:      PUTIDS(present ? 1 : 0, kObj_Device);
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            SIZE(sizeof(AudioObjectID));
            AudioObjectID found = kAudioObjectUnknown;
            if (present && q && qsz == sizeof(CFStringRef) && *(CFStringRef *)q) {
                CFStringRef mine = cfstr("slfw:%012llx", gInfo.guid);
                if (CFEqual(*(CFStringRef *)q, mine)) found = kObj_Device;
                CFRelease(mine);
            }
            *(AudioObjectID *)out = found;
            return noErr;
        }
        case kAudioPlugInPropertyResourceBundle:  PUTCF(CFSTR(""));
        case kAudioObjectPropertyCustomPropertyInfoList: SIZE(0); return noErr;
        }
        return kAudioHardwareUnknownPropertyError;
    }

    if (obj == kObj_Device) {
        if (!present) return kAudioHardwareBadObjectError;
        bool input = scope == kAudioObjectPropertyScopeInput;
        bool output = scope == kAudioObjectPropertyScopeOutput;
        switch (sel) {
        case kAudioObjectPropertyBaseClass:       PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:           PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:           PUT(AudioObjectID, kObj_PlugIn);
        case kAudioObjectPropertyName:            PUTCF(cfstr("%s FireWire", gInfo.nickname));
        case kAudioObjectPropertyManufacturer:    PUTCF(CFSTR("PreSonus"));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:         PUTIDS(output ? 0 : 1, kObj_Stream);
        case kAudioObjectPropertyControlList:     PUTIDS(0, 0);
        case kAudioObjectPropertyCustomPropertyInfoList: SIZE(0); return noErr;
        case kAudioDevicePropertyDeviceUID:       PUTCF(cfstr("slfw:%012llx", gInfo.guid));
        case kAudioDevicePropertyModelUID:        PUTCF(CFSTR("slfw:StudioLiveAI"));
        case kAudioDevicePropertyTransportType:   PUT(UInt32, kAudioDeviceTransportTypeFireWire);
        case kAudioDevicePropertyRelatedDevices:  PUTIDS(1, kObj_Device);
        case kAudioDevicePropertyClockDomain:     PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive:   PUT(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: PUT(UInt32, atomic_load(&gRunning) ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:       PUT(UInt32, input ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, 0);
        case kAudioDevicePropertyLatency:         PUT(UInt32, input ? kLatencyFrames : 0);
        case kAudioDevicePropertySafetyOffset:    PUT(UInt32, input ? kSafetyOffset : 0);
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, (Float64)gInfo.sample_rate);
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            SIZE(sizeof(AudioValueRange));
            AudioValueRange r = { gInfo.sample_rate, gInfo.sample_rate };
            *(AudioValueRange *)out = r;
            return noErr;
        }
        case kAudioDevicePropertyIsHidden:        PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, kZeroTSPeriod);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            SIZE(2 * sizeof(UInt32));
            ((UInt32 *)out)[0] = 1; ((UInt32 *)out)[1] = 2;
            return noErr;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            if (!input) return kAudioHardwareUnknownPropertyError;
            SIZE(offsetof(AudioChannelLayout, mChannelDescriptions) + ch * sizeof(AudioChannelDescription));
            AudioChannelLayout *l = out;
            memset(l, 0, *outsz);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = ch;
            for (UInt32 i = 0; i < ch; i++) l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Unknown;
            return noErr;
        }
        case kAudioObjectPropertyElementName:
            /* This is what puts "CH1", "AUX3" on Studio One's input list. */
            if (!input || a->mElement < 1 || a->mElement > ch) return kAudioHardwareUnknownPropertyError;
            PUTCF(cfstr("%s", gInfo.names[a->mElement - 1]));
        }
        return kAudioHardwareUnknownPropertyError;
    }

    if (obj == kObj_Stream) {
        if (!present) return kAudioHardwareBadObjectError;
        switch (sel) {
        case kAudioObjectPropertyBaseClass:       PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:           PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:           PUT(AudioObjectID, kObj_Device);
        case kAudioObjectPropertyName:            PUTCF(CFSTR("Console"));
        case kAudioObjectPropertyOwnedObjects:    PUTIDS(0, 0);
        case kAudioObjectPropertyCustomPropertyInfoList: SIZE(0); return noErr;
        case kAudioStreamPropertyIsActive:        PUT(UInt32, 1);
        case kAudioStreamPropertyDirection:       PUT(UInt32, 1);
        case kAudioStreamPropertyTerminalType:    PUT(UInt32, kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, 1);
        case kAudioStreamPropertyLatency:         PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:  PUT(AudioStreamBasicDescription, stream_format());
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            SIZE(sizeof(AudioStreamRangedDescription));
            AudioStreamRangedDescription r = { stream_format(), { gInfo.sample_rate, gInfo.sample_rate } };
            *(AudioStreamRangedDescription *)out = r;
            return noErr;
        }
        }
        return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareBadObjectError;
#undef SIZE
#undef PUT
#undef PUTCF
#undef PUTIDS
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                           const AudioObjectPropertyAddress *a)
{
    UInt32 sz = 0;
    return prop(obj, a, 0, NULL, 0, &sz, NULL) == noErr;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                   const AudioObjectPropertyAddress *a, Boolean *outSettable)
{
    UInt32 sz = 0;
    OSStatus r = prop(obj, a, 0, NULL, 0, &sz, NULL);
    if (r != noErr) return r;
    /* The console owns its clock. Rate and format writes are accepted only when
     * they ask for what it already is, so hosts that set them still work. */
    *outSettable = (obj == kObj_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) ||
                   (obj == kObj_Stream && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                                           a->mSelector == kAudioStreamPropertyPhysicalFormat ||
                                           a->mSelector == kAudioStreamPropertyIsActive));
    return noErr;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                    const AudioObjectPropertyAddress *a, UInt32 qsz, const void *q,
                                    UInt32 *outSize)
{
    return prop(obj, a, qsz, q, 0, outSize, NULL);
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsz, const void *q,
                                UInt32 insz, UInt32 *outSize, void *outData)
{
    if (!outData) return kAudioHardwareIllegalOperationError;
    return prop(obj, a, qsz, q, insz, outSize, outData);
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid,
                                const AudioObjectPropertyAddress *a, UInt32 qsz, const void *q,
                                UInt32 insz, const void *data)
{
    if (obj == kObj_Device && a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (insz < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return fabs(*(const Float64 *)data - gInfo.sample_rate) < 0.5 ? noErr : kAudioDeviceUnsupportedFormatError;
    }
    if (obj == kObj_Stream && (a->mSelector == kAudioStreamPropertyVirtualFormat ||
                               a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        if (insz < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *f = data;
        AudioStreamBasicDescription mine = stream_format();
        return (fabs(f->mSampleRate - mine.mSampleRate) < 0.5 && f->mChannelsPerFrame == mine.mChannelsPerFrame &&
                f->mFormatID == mine.mFormatID) ? noErr : kAudioDeviceUnsupportedFormatError;
    }
    if (obj == kObj_Stream && a->mSelector == kAudioStreamPropertyIsActive) return noErr;
    return kAudioHardwareUnsupportedOperationError;
}

/* -------------------------------------------------------------------- I/O */

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    if (dev != kObj_Device || !atomic_load(&gPresent)) return kAudioHardwareBadObjectError;
    if (atomic_fetch_add(&gClients, 1) > 0) return noErr;

    __block OSStatus st = noErr;
    bool ran = on_fw(^{
        io_service_t svc = slfw_find_service(gInfo.guid);
        if (!svc) { st = kAudioHardwareNotRunningError; return; }
        char err[256];
        slfw *fw = slfw_open(svc, gFWRunLoop, fwlog, err, sizeof err);
        IOObjectRelease(svc);
        if (!fw) { os_log_error(gLog, "StartIO: open failed: %{public}s", err); st = kAudioHardwareUnspecifiedError; return; }
        if (slfw_start(fw) != 0) { slfw_close(fw); st = kAudioHardwareUnspecifiedError; return; }
        /* The rate stays what Core Audio already has; health() corrects it from
         * the stream's cadence within a second if the console disagrees. */
        settle_rate(fw);
        atomic_store(&gNewStream, true);
        atomic_store(&gFW, fw);
    });
    if (!ran) st = kAudioHardwareUnspecifiedError;
    if (st != noErr) { atomic_fetch_sub(&gClients, 1); return st; }

    /* Core Audio asks for a timeline as soon as this returns, so wait for the
     * first packets to give the clock something to stand on. The call-backs
     * run on the FireWire thread, which is free while we wait here. */
    atomic_store(&gHaveClock, false);
    gZSample = -1;
    for (int i = 0; i < 300; i++) {
        double tpf, bh; uint64_t bf; uint32_t seed;
        slfw *fw = atomic_load(&gFW);
        if (fw && slfw_clock(fw, &tpf, &bf, &bh, &seed)) {
            gLastTpf = tpf; gLastBaseFrame = bf; gLastBaseHost = bh; gLastSeed = seed;
            atomic_store(&gHaveClock, true);
            break;
        }
        usleep(10000);
    }
    if (!atomic_load(&gHaveClock)) {
        os_log_error(gLog, "StartIO: no packets within 3 s");
        on_fw(^{ release_stream("I/O stopped"); });
        atomic_fetch_sub(&gClients, 1);
        return kAudioHardwareUnspecifiedError;
    }
    atomic_store(&gUnderruns, 0);
    atomic_store(&gRunning, true);
    notify(kObj_Device, kAudioDevicePropertyDeviceIsRunning);
    os_log(gLog, "I/O started");
    return noErr;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client)
{
    if (dev != kObj_Device) return kAudioHardwareBadObjectError;
    UInt32 n = atomic_load(&gClients);
    if (n == 0) return noErr;
    if (atomic_fetch_sub(&gClients, 1) != 1) return noErr;
    atomic_store(&gRunning, false);
    on_fw(^{ release_stream("I/O stopped"); });
    notify(kObj_Device, kAudioDevicePropertyDeviceIsRunning);
    os_log(gLog, "I/O stopped (read underruns %llu)", atomic_load(&gUnderruns));
    return noErr;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                 Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed)
{
    double tpf, bh; uint64_t bf; uint32_t seed;
    atomic_fetch_add(&gInIO, 1);
    slfw *fw = atomic_load(&gFW);
    bool live = fw && slfw_clock(fw, &tpf, &bf, &bh, &seed);
    atomic_fetch_sub(&gInIO, 1);
    if (live) {
        gLastTpf = tpf; gLastBaseFrame = bf; gLastBaseHost = bh; gLastSeed = seed;
    } else if (atomic_load(&gHaveClock)) {
        tpf = gLastTpf; bf = gLastBaseFrame; bh = gLastBaseHost; seed = gLastSeed;
    } else {
        *outSampleTime = 0; *outHostTime = mach_absolute_time(); *outSeed = 1;
        return noErr;
    }
    double now = (double)mach_absolute_time();
    double fnow = (double)bf + (now - bh) / tpf;
    if (fnow < 0) fnow = 0;
    double fz = floor(fnow / kZeroTSPeriod) * kZeroTSPeriod;
    double hz = bh + (fz - (double)bf) * tpf;          /* where the fit puts it */

    if (gZSample < 0 || seed != gZSeed) {
        gZSample = fz; gZHost = hz; gZSeed = seed;
    } else if (fz > gZSample) {
        /* Extrapolate from the latch at the current rate, then steer toward the
         * fit by no more than 200 ppm of the elapsed span. Rate is matched by
         * tpf; this only walks out phase error, so the timeline stays strictly
         * monotonic and a 1 ms offset is absorbed in about five seconds. */
        double span = fz - gZSample;
        double pred = gZHost + span * tpf;
        double err = hz - pred, lim = span * tpf * 200e-6;
        if (err > lim) err = lim;
        if (err < -lim) err = -lim;
        gZSample = fz;
        gZHost = pred + err;
    }
    /* fz < gZSample means the fit stepped back across a boundary: keep the
     * latch, never report a sample time Core Audio has already moved past. */
    *outSampleTime = gZSample;
    *outHostTime = (UInt64)gZHost;
    *outSeed = gZSeed;
    return noErr;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                  UInt32 op, Boolean *outWillDo, Boolean *outInPlace)
{
    *outWillDo = (op == kAudioServerPlugInIOOperationReadInput);
    *outInPlace = true;
    return noErr;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                                 UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{ return noErr; }

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream,
                              UInt32 client, UInt32 op, UInt32 frames,
                              const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary)
{
    if (op != kAudioServerPlugInIOOperationReadInput) return noErr;
    UInt32 ch = gInfo.channels;
    Float32 *out = main;
    atomic_fetch_add(&gInIO, 1);
    slfw *fw = atomic_load(&gFW);
    if (!fw || frames > kMaxIOFrames) {
        atomic_fetch_sub(&gInIO, 1);
        memset(out, 0, (size_t)frames * ch * sizeof(Float32));
        return noErr;
    }

    double t = cycle->mInputTime.mSampleTime;
    uint64_t pos = t > 0 ? (uint64_t)llround(t) : 0;
    unsigned real = slfw_read(fw, pos, gScratch, frames);
    atomic_fetch_sub(&gInIO, 1);
    if (real < frames) atomic_fetch_add(&gUnderruns, frames - real);

    const Float32 k = 1.0f / 2147483648.0f;
    size_t n = (size_t)frames * ch;
    for (size_t i = 0; i < n; i++) out[i] = (Float32)gScratch[i] * k;
    return noErr;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client,
                               UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{ return noErr; }

/* -------------------------------------------------------------- plumbing */

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host)
{
    gHost = host;
    gScratch = calloc((size_t)kMaxIOFrames * SLFW_MAX_CH, sizeof(int32_t));
    if (!gScratch) return kAudioHardwareUnspecifiedError;

    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, fw_thread, ready) != 0) return kAudioHardwareUnspecifiedError;
    dispatch_semaphore_wait(ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    os_log(gLog, "initialized; console %{public}s", atomic_load(&gPresent) ? "present" : "not found yet");
    return noErr;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc,
                             const AudioServerPlugInClientInfo *ci, AudioObjectID *outID)
{ return kAudioHardwareUnsupportedOperationError; }
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev)
{ return kAudioHardwareUnsupportedOperationError; }
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{ return noErr; }
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci)
{ return noErr; }
/* Core Audio has stopped I/O for the change publish_rate requested. */
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{
    unsigned rate = atomic_exchange(&gPendingRate, 0);
    if (dev != kObj_Device || !rate) return noErr;
    pthread_mutex_lock(&gLock);
    gInfo.sample_rate = rate;
    pthread_mutex_unlock(&gLock);
    notify(kObj_Device, kAudioDevicePropertyNominalSampleRate);
    notify(kObj_Device, kAudioDevicePropertyAvailableNominalSampleRates);
    notify(kObj_Stream, kAudioStreamPropertyVirtualFormat);
    notify(kObj_Stream, kAudioStreamPropertyPhysicalFormat);
    notify(kObj_Stream, kAudioStreamPropertyAvailableVirtualFormats);
    notify(kObj_Stream, kAudioStreamPropertyAvailablePhysicalFormats);
    os_log(gLog, "now advertising %u Hz", rate);
    return noErr;
}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info)
{ return noErr; }

static HRESULT QueryInterface(void *d, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *d)  { return atomic_fetch_add(&gRefCount, 1) + 1; }
static ULONG Release(void *d) { ULONG n = atomic_load(&gRefCount); if (n > 0) atomic_fetch_sub(&gRefCount, 1); return n ? n - 1 : 0; }

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release,
    Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange,
    HasProperty, IsPropertySettable, GetPropertyDataSize, GetPropertyData, SetPropertyData,
    StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

static HRESULT QueryInterface(void *d, REFIID uuid, LPVOID *out)
{
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(d);
        *out = gDriverRef;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}

__attribute__((visibility("default")))
void *SLFW_Create(CFAllocatorRef allocator, CFUUIDRef type)
{
    if (!gLog) gLog = os_log_create("n12n.presonus-adapter", "driver");
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? gDriverRef : NULL;
}
