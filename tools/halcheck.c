// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * halcheck — load the built driver bundle in an ordinary process and drive it
 * the way Core Audio would, before it goes anywhere near coreaudiod.
 *
 *   halcheck [build/StudioLiveFW.driver] [seconds]
 *
 * A plug-in that crashes inside Core Audio's driver service takes audio down
 * for the whole Mac. This exercises the same entry points — factory, COM,
 * Initialize, the property model, StartIO, GetZeroTimeStamp, DoIOOperation,
 * StopIO — with a fake host, and checks the results. It does NOT test the
 * sandbox; only installing does that.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static AudioServerPlugInDriverRef D;
static unsigned g_changes;

static OSStatus h_changed(AudioServerPlugInHostRef h, AudioObjectID o, UInt32 n, const AudioObjectPropertyAddress *a)
{ g_changes += n; return noErr; }
static OSStatus h_copy(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *out)
{ *out = NULL; return kAudioHardwareUnknownPropertyError; }
static OSStatus h_write(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d) { return noErr; }
static OSStatus h_delete(AudioServerPlugInHostRef h, CFStringRef k) { return noErr; }
static OSStatus h_reqcfg(AudioServerPlugInHostRef h, AudioObjectID d, UInt64 a, void *i) { return noErr; }
static const struct AudioServerPlugInHostInterface gHostIf = { h_changed, h_copy, h_write, h_delete, h_reqcfg };

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static OSStatus get(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc,
                    AudioObjectPropertyElement el, UInt32 sz, void *out, UInt32 *got)
{
    AudioObjectPropertyAddress a = { sel, sc, el };
    UInt32 g = sz;
    OSStatus r = (*D)->GetPropertyData(D, o, getpid(), &a, 0, NULL, sz, &g, out);
    if (got) *got = g;
    return r;
}

static void str(const char *label, AudioObjectID o, AudioObjectPropertySelector sel,
                AudioObjectPropertyScope sc, AudioObjectPropertyElement el)
{
    CFStringRef s = NULL; char b[256] = "(none)";
    OSStatus r = get(o, sel, sc, el, sizeof s, &s, NULL);
    if (r == noErr && s) { CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8); CFRelease(s); }
    printf("  %-24s %s\n", label, b);
    CHECK(r == noErr, "%s returned %d", label, (int)r);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "build/StudioLiveFW.driver";
    double secs = argc > 2 ? atof(argv[2]) : 5;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), true);
    CFBundleRef b = CFBundleCreate(NULL, url);
    if (!b || !CFBundleLoadExecutable(b)) { fprintf(stderr, "cannot load %s\n", path); return 1; }
    void *(*create)(CFAllocatorRef, CFUUIDRef) = CFBundleGetFunctionPointerForName(b, CFSTR("SLFW_Create"));
    if (!create) { fprintf(stderr, "factory SLFW_Create not exported\n"); return 1; }
    D = create(NULL, kAudioServerPlugInTypeUUID);
    if (!D) { fprintf(stderr, "factory refused the AudioServerPlugIn type\n"); return 1; }
    CHECK(create(NULL, kAudioServerPlugInDriverInterfaceUUID) == NULL, "factory accepted a wrong type");

    LPVOID qi = NULL;
    CHECK((*D)->QueryInterface(D, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &qi) == S_OK && qi == D,
          "QueryInterface for the driver interface");
    CHECK((*D)->Initialize(D, &gHostIf) == noErr, "Initialize");

    printf("plug-in\n");
    AudioObjectID devs[4]; UInt32 got;
    get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain, sizeof devs, devs, &got);
    UInt32 ndev = got / sizeof(AudioObjectID);
    printf("  %-24s %u\n", "devices", ndev);
    if (!ndev) { printf("  FAIL: no device published — is the console on the bus?\n"); return 1; }
    AudioObjectID dev = devs[0];

    printf("device %u\n", dev);
    str("name", dev, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0);
    str("manufacturer", dev, kAudioObjectPropertyManufacturer, kAudioObjectPropertyScopeGlobal, 0);
    str("UID", dev, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0);

    CFStringRef uid = NULL;
    get(dev, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0, sizeof uid, &uid, NULL);
    AudioObjectID back = 0;
    AudioObjectPropertyAddress ta = { kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 tsz = sizeof back;
    (*D)->GetPropertyData(D, kAudioObjectPlugInObject, getpid(), &ta, sizeof uid, &uid, sizeof back, &tsz, &back);
    CHECK(back == dev, "TranslateUIDToDevice gave %u, want %u", back, dev);
    if (uid) CFRelease(uid);

    Float64 rate = 0; UInt32 zper = 0, safety = 0, lat = 0, transport = 0;
    get(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0, sizeof rate, &rate, NULL);
    get(dev, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal, 0, sizeof zper, &zper, NULL);
    get(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, 0, sizeof safety, &safety, NULL);
    get(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput, 0, sizeof lat, &lat, NULL);
    get(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, 0, sizeof transport, &transport, NULL);
    printf("  %-24s %.0f Hz\n  %-24s %u frames\n  %-24s %u / latency %u\n  %-24s '%.4s'\n",
           "nominal rate", rate, "zero-timestamp period", zper, "input safety offset", safety, lat,
           "transport", (char *)&(UInt32){ CFSwapInt32HostToBig(transport) });
    CHECK(rate > 0, "nominal rate");

    AudioObjectID streams[4], outstreams[4];
    get(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, 0, sizeof streams, streams, &got);
    CHECK(got == sizeof(AudioObjectID), "one input stream, got %u bytes", got);
    get(dev, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, 0, sizeof outstreams, outstreams, &got);
    CHECK(got == 0, "no output streams, got %u bytes", got);
    AudioObjectID stream = streams[0];

    AudioStreamBasicDescription f;
    get(stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, 0, sizeof f, &f, NULL);
    printf("stream %u\n  %-24s %.0f Hz, %u ch, %u-bit float\n", stream, "format", f.mSampleRate,
           f.mChannelsPerFrame, f.mBitsPerChannel);
    CHECK(fabs(f.mSampleRate - rate) < 0.5, "stream rate matches device");
    UInt32 ch = f.mChannelsPerFrame;
    str("channel 1 name", dev, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 1);
    str("channel 24 name", dev, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 24);
    if (ch >= 25) str("channel 25 name", dev, kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, 25);
    AudioObjectPropertyAddress bad = { kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, ch + 1 };
    CHECK(!(*D)->HasProperty(D, dev, getpid(), &bad), "element name beyond the last channel should not exist");

    /* ----- I/O, paced like Core Audio: read behind "now" by the safety offset ----- */
    printf("I/O for %.0f s\n", secs);
    uint64_t t0 = mach_absolute_time();
    OSStatus r = (*D)->StartIO(D, dev, 1);
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double ns_per_tick = (double)tb.numer / tb.denom;
    printf("  %-24s %d after %.0f ms\n", "StartIO", (int)r, (mach_absolute_time() - t0) * ns_per_tick / 1e6);
    if (r != noErr) { printf("  FAIL: StartIO\n"); return 1; }

    const UInt32 N = 512;
    Float32 *buf = calloc((size_t)N * ch, sizeof(Float32));
    double tpf_nom = 1e9 / rate / ns_per_tick;
    Float64 first_s = -1, last_s = -1, prev_s = -1; UInt64 first_h = 0, last_h = 0, prev_h = 0, seed0 = 0;
    unsigned cycles = 0, empty = 0, zbad = 0, seeds = 0;
    double wobble = 0;
    double peak[64] = { 0 };
    uint64_t end = mach_absolute_time() + (uint64_t)(secs * 1e9 / ns_per_tick);

    while (mach_absolute_time() < end) {
        Float64 zs; UInt64 zh, zseed;
        (*D)->GetZeroTimeStamp(D, dev, 1, &zs, &zh, &zseed);
        if (fmod(zs, zper) != 0) { zbad++; printf("  zts: sample %.0f is not a period multiple\n", zs); }
        if (prev_s >= 0) {
            if (zs < prev_s) {
                zbad++; printf("  zts: sample time went back %.0f -> %.0f\n", prev_s, zs);
            } else if (zs == prev_s && zh != prev_h) {
                double w = fabs((double)zh - (double)prev_h) * ns_per_tick / 1e6;
                if (w > wobble) wobble = w;
                zbad++;
            } else if (zs > prev_s && zh <= prev_h) {
                zbad++; printf("  zts: host time did not advance with sample time\n");
            }
        }
        if (!seed0) seed0 = zseed; else if (zseed != seed0) seeds++;
        if (zs != prev_s) {
            if (first_s < 0) { first_s = zs; first_h = zh; }
            last_s = zs; last_h = zh;
        }
        prev_s = zs; prev_h = zh;

        double cur = zs + (double)(mach_absolute_time() - zh) / tpf_nom;
        double pos = floor(cur) - safety - N;
        if (pos >= 0) {
            AudioServerPlugInIOCycleInfo ci;
            memset(&ci, 0, sizeof ci);
            ci.mIOCycleCounter = cycles;
            ci.mNominalIOBufferFrameSize = N;
            ci.mInputTime.mSampleTime = pos;
            ci.mInputTime.mFlags = kAudioTimeStampSampleTimeValid;
            (*D)->BeginIOOperation(D, dev, 1, kAudioServerPlugInIOOperationReadInput, N, &ci);
            (*D)->DoIOOperation(D, dev, stream, 1, kAudioServerPlugInIOOperationReadInput, N, &ci, buf, NULL);
            (*D)->EndIOOperation(D, dev, 1, kAudioServerPlugInIOOperationReadInput, N, &ci);
            bool allzero = true;
            for (size_t i = 0; i < (size_t)N * ch; i++) {
                if (buf[i] != 0) allzero = false;
                unsigned c = i % ch;
                if (c < 64 && fabs(buf[i]) > peak[c]) peak[c] = fabs(buf[i]);
            }
            if (allzero) empty++;
            cycles++;
        }
        usleep((useconds_t)(N * 1e6 / rate));
    }
    r = (*D)->StopIO(D, dev, 1);
    CHECK(r == noErr, "StopIO returned %d", (int)r);

    double measured = 0;
    if (last_s > first_s)
        measured = (last_s - first_s) / ((double)(last_h - first_h) * ns_per_tick / 1e9);
    printf("  %-24s %u (%u returned all-zero buffers)\n", "read cycles", cycles, empty);
    printf("  %-24s %u malformed, %u seed changes, max host wobble at a fixed sample %.3f ms\n",
           "zero timestamps", zbad, seeds, wobble);
    printf("  %-24s %.2f Hz (%+.0f ppm from nominal)\n", "rate from timestamps", measured,
           measured ? (measured / rate - 1) * 1e6 : 0);
    printf("  %-24s CH1 %.1f dBFS, CH24 %.1f dBFS%s\n", "peaks", 20 * log10(peak[0] + 1e-12),
           20 * log10(peak[23] + 1e-12), ch > 24 ? "" : "");
    CHECK(cycles > 0, "no read cycles ran");
    CHECK(empty <= cycles / 20, "too many empty buffers: %u of %u", empty, cycles);
    CHECK(zbad == 0, "malformed zero timestamps");
    CHECK(seeds == 0, "seed changed during a steady stream");
    CHECK(measured == 0 || fabs(measured / rate - 1) < 0.001, "timeline rate is off nominal by over 0.1%%");

    printf("\n%s (%d failure%s, %u property notifications)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s", g_changes);
    return failures ? 1 : 0;
}
