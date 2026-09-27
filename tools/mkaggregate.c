// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mkaggregate — make the one device Studio One 5 needs.
 *
 *   mkaggregate "<output device name>"   e.g. "MacBook Pro Speakers"
 *   mkaggregate --list
 *   mkaggregate --sync
 *
 * Studio One 5 on the Mac picks a single audio device for both recording and
 * playback. The console driver is input-only, so this creates a system-wide
 * aggregate: every console channel as inputs, plus the named device's outputs
 * for playback. The console is the clock master, so recorded audio is never
 * resampled; the output device is drift-compensated instead.
 *
 * The output device is the one whose whole name matches, ignoring case, or else
 * the only one whose name contains the argument. Several partial matches is an
 * error that lists them, never a guess.
 *
 * An aggregate follows its clock device when that device changes rate on its
 * own, but leaves its other sub-devices where they were: when the console moves
 * between 44.1 and 48 kHz, the output device stays at the old rate. Setting the
 * aggregate's own rate does reach every sub-device, even when the value is the
 * one it already reports, so --sync sets the aggregate to the rate the console
 * already has. The driver accepts a rate write that asks for its current rate.
 * --sync also turns drift compensation back on for the output device; see
 * drift_off for why creating the aggregate does not do that on its own.
 */
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioHardware.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define AGG_UID CFSTR("slfw:aggregate")

static const AudioObjectPropertyAddress kRate = { kAudioDevicePropertyNominalSampleRate,
                                                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

static CFStringRef cfprop(AudioObjectID o, AudioObjectPropertySelector sel)
{
    CFStringRef s = NULL; UInt32 sz = sizeof s;
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, &s) == noErr ? s : NULL;
}

static void name_of(AudioObjectID o, char *buf, size_t n)
{
    CFStringRef s = cfprop(o, kAudioObjectPropertyName);
    snprintf(buf, n, "?");
    if (s) { CFStringGetCString(s, buf, n, kCFStringEncodingUTF8); CFRelease(s); }
}

static UInt32 channels(AudioObjectID o, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain };
    UInt32 sz = 0, n = 0;
    if (AudioObjectGetPropertyDataSize(o, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioBufferList *bl = malloc(sz);
    if (AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) n += bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

static Float64 nominal_rate(AudioObjectID o)
{
    Float64 r = 0; UInt32 sz = sizeof r;
    return AudioObjectGetPropertyData(o, &kRate, 0, NULL, &sz, &r) == noErr ? r : 0;
}

static OSStatus set_rate(AudioObjectID o, Float64 r)
{
    return AudioObjectSetPropertyData(o, &kRate, 0, NULL, sizeof r, &r);
}

/* Counts the aggregate's present sub-devices whose rate differs from its own,
 * naming each one when asked to. */
static int mismatches(AudioObjectID agg, bool print)
{
    AudioObjectPropertyAddress a = { kAudioAggregateDevicePropertyActiveSubDeviceList,
                                     kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID subs[16]; UInt32 sz = sizeof subs;
    if (AudioObjectGetPropertyData(agg, &a, 0, NULL, &sz, subs) != noErr) return 0;
    Float64 want = nominal_rate(agg);
    int bad = 0;
    for (UInt32 i = 0; i < sz / sizeof *subs; i++) {
        Float64 r = nominal_rate(subs[i]);
        if (fabs(r - want) < 0.5) continue;
        if (print) {
            char nm[256]; name_of(subs[i], nm, sizeof nm);
            printf("  %s is at %.0f Hz; the aggregate runs at %.0f Hz\n", nm, r, want);
        }
        bad++;
    }
    return bad;
}

/* Drift compensation, per sub-device other than the console, which is the
 * clock. The composition key kAudioSubDeviceDriftCompensationKey is accepted by
 * AudioHardwareCreateAggregateDevice and then ignored: the aggregate built with
 * it read back drift 0 on the output device (2026-09-26). Uncompensated, a USB
 * interface ~90 ppm off the console slips a whole buffer every 20,000 I/O
 * cycles — an overload and a click every 2 minutes at 256 frames, whatever the
 * buffer size. So it is set as a property on the live sub-device instead, and
 * checked. It takes effect the next time the aggregate starts I/O. Returns how
 * many sub-devices are still uncompensated; fixes them first when asked to. */
static int drift_off(AudioObjectID agg, bool fix, bool print)
{
    AudioObjectPropertyAddress oa = { kAudioObjectPropertyOwnedObjects,
                                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress ca = { kAudioObjectPropertyClass,
                                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress da = { kAudioSubDevicePropertyDriftCompensation,
                                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID objs[32]; UInt32 sz = sizeof objs;
    if (AudioObjectGetPropertyData(agg, &oa, 0, NULL, &sz, objs) != noErr) return 0;
    int off = 0;
    for (UInt32 i = 0; i < sz / sizeof *objs; i++) {
        AudioClassID cls = 0; UInt32 csz = sizeof cls;
        if (AudioObjectGetPropertyData(objs[i], &ca, 0, NULL, &csz, &cls) != noErr ||
            cls != kAudioSubDeviceClassID) continue;
        CFStringRef uid = cfprop(objs[i], kAudioDevicePropertyDeviceUID);
        bool clock = uid && CFStringHasPrefix(uid, CFSTR("slfw:"));
        if (uid) CFRelease(uid);
        if (clock) continue;
        UInt32 on = 0, dsz = sizeof on;
        AudioObjectGetPropertyData(objs[i], &da, 0, NULL, &dsz, &on);
        if (!on && fix) {
            UInt32 one = 1;
            AudioObjectSetPropertyData(objs[i], &da, 0, NULL, sizeof one, &one);
            for (int k = 0; k < 20 && !on; k++) {   /* applied asynchronously, like the rate */
                usleep(50000);
                dsz = sizeof on;
                AudioObjectGetPropertyData(objs[i], &da, 0, NULL, &dsz, &on);
            }
        }
        if (on) continue;
        if (print) {
            char nm[256]; name_of(objs[i], nm, sizeof nm);
            printf("  %s is not drift-compensated\n", nm);
        }
        off++;
    }
    return off;
}

static int sync_drift(AudioObjectID agg)
{
    if (!drift_off(agg, false, false)) { printf("the output device is drift-compensated\n"); return 0; }
    if (drift_off(agg, true, true)) {
        fprintf(stderr, "could not turn drift compensation on\n");
        return 1;
    }
    printf("drift compensation is now on; it applies from the next time Studio One starts audio\n");
    return 0;
}

/* Brings every sub-device to the aggregate's rate, which is its clock's. */
static int sync_rates(AudioObjectID agg)
{
    Float64 rate = nominal_rate(agg);
    if (!mismatches(agg, true)) { printf("every sub-device is at %.0f Hz\n", rate); return 0; }
    OSStatus r = set_rate(agg, rate);
    for (int i = 0; i < 30 && mismatches(agg, false); i++) usleep(100000);   /* sub-devices change rate asynchronously */
    if (r != noErr || mismatches(agg, true)) {
        fprintf(stderr, "could not bring every sub-device to %.0f Hz (%d)\n", rate, (int)r);
        return 1;
    }
    printf("every sub-device is now at %.0f Hz\n", rate);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mkaggregate \"<output device name>\" | --list | --sync\n"); return 2; }
    bool list = !strcmp(argv[1], "--list"), sync = !strcmp(argv[1], "--sync");
    const char *want = list || sync ? NULL : argv[1];

    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioObjectID ids[128]; UInt32 sz = sizeof ids;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &sz, ids);
    UInt32 n = sz / sizeof(AudioObjectID);

    AudioObjectID existing = 0, console = 0, exact = 0, partial[16];
    int npartial = 0;
    for (UInt32 i = 0; i < n; i++) {
        CFStringRef uid = cfprop(ids[i], kAudioDevicePropertyDeviceUID);
        char nm[256]; name_of(ids[i], nm, sizeof nm);
        UInt32 outs = channels(ids[i], kAudioObjectPropertyScopeOutput);
        if (list)
            printf("  %-40s in %2u  out %2u  %6.0f Hz\n", nm, channels(ids[i], kAudioObjectPropertyScopeInput), outs,
                   nominal_rate(ids[i]));
        if (uid && CFEqual(uid, AGG_UID)) existing = ids[i];
        else if (uid && CFStringHasPrefix(uid, CFSTR("slfw:"))) console = ids[i];
        else if (want && uid && outs) {
            if (!strcasecmp(nm, want)) exact = ids[i];
            else if (strcasestr(nm, want) && npartial < 16) partial[npartial++] = ids[i];
        }
        if (uid) CFRelease(uid);
    }

    if (list) {
        if (existing && mismatches(existing, false)) {
            printf("\nthe StudioLive aggregate's sub-devices are at different rates:\n");
            mismatches(existing, true);
            printf("fix it with: mkaggregate --sync\n");
        }
        if (existing && drift_off(existing, false, false)) {
            printf("\nthe StudioLive aggregate will click every minute or two:\n");
            drift_off(existing, false, true);
            printf("fix it with: mkaggregate --sync\n");
        }
        return 0;
    }
    if (sync) {
        if (!existing) { fprintf(stderr, "there is no StudioLive aggregate; make one with mkaggregate \"<output device>\"\n"); return 1; }
        return sync_drift(existing) | sync_rates(existing);
    }

    AudioObjectID out_dev = exact;
    if (!out_dev && npartial > 1) {
        fprintf(stderr, "\"%s\" matches %d output devices; give one of these names in full:\n", want, npartial);
        for (int i = 0; i < npartial; i++) { char nm[256]; name_of(partial[i], nm, sizeof nm); fprintf(stderr, "  %s\n", nm); }
        return 1;
    }
    if (!out_dev && npartial) out_dev = partial[0];
    if (!console) { fprintf(stderr, "the console driver is not visible to Core Audio\n"); return 1; }
    if (!out_dev) { fprintf(stderr, "no output device matching \"%s\" (try --list)\n", want); return 1; }

    if (existing) {
        OSStatus r = AudioHardwareDestroyAggregateDevice(existing);
        printf("replaced the previous aggregate (%d)\n", (int)r);
    }

    CFStringRef console_uid = cfprop(console, kAudioDevicePropertyDeviceUID);
    CFStringRef out_uid = cfprop(out_dev, kAudioDevicePropertyDeviceUID);

    CFMutableDictionaryRef sub_console = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub_console, CFSTR(kAudioSubDeviceUIDKey), console_uid);
    CFMutableDictionaryRef sub_out = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub_out, CFSTR(kAudioSubDeviceUIDKey), out_uid);
    CFTypeRef subs[2] = { sub_console, sub_out };
    CFArrayRef sublist = CFArrayCreate(NULL, subs, 2, &kCFTypeArrayCallBacks);

    char outn[200]; name_of(out_dev, outn, sizeof outn);
    char aggn[256];
    snprintf(aggn, sizeof aggn, "StudioLive + %s", outn);
    CFStringRef agg_name = CFStringCreateWithCString(NULL, aggn, kCFStringEncodingUTF8);

    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), agg_name);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), AGG_UID);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), sublist);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), console_uid);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), kCFBooleanFalse);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), kCFBooleanFalse);

    AudioObjectID agg = 0;
    OSStatus r = AudioHardwareCreateAggregateDevice(desc, &agg);
    if (r != noErr) { fprintf(stderr, "AudioHardwareCreateAggregateDevice failed: %d\n", (int)r); return 1; }
    usleep(500000);   /* the aggregate finishes building its sub-device list asynchronously */
    printf("created \"%s\" (device %u): %u inputs, %u outputs, clock from the console\n",
           aggn, agg, channels(agg, kAudioObjectPropertyScopeInput), channels(agg, kAudioObjectPropertyScopeOutput));
    return sync_drift(agg) | sync_rates(agg);   /* the output device joins at whatever rate it was left at */
}
