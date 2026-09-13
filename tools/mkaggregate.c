// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * mkaggregate — make the one device Studio One 5 needs.
 *
 *   mkaggregate "<output device name substring>"   e.g. "MacBook Pro Speakers"
 *   mkaggregate --list
 *
 * Studio One 5 on the Mac picks a single audio device for both recording and
 * playback. The console driver is input-only, so this creates a system-wide
 * aggregate: every console channel as inputs, plus the named device's outputs
 * for playback. The console is the clock master, so recorded audio is never
 * resampled; the output device is drift-compensated instead.
 */
#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/AudioHardware.h>
#include <stdio.h>
#include <string.h>

#define AGG_UID CFSTR("slfw:aggregate")

static CFStringRef cfprop(AudioObjectID o, AudioObjectPropertySelector sel)
{
    CFStringRef s = NULL; UInt32 sz = sizeof s;
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, &s) == noErr ? s : NULL;
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

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mkaggregate \"<output device name>\" | --list\n"); return 2; }

    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &da, 0, NULL, &sz);
    AudioObjectID ids[128];
    if (sz > sizeof ids) sz = sizeof ids;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &sz, ids);
    UInt32 n = sz / sizeof(AudioObjectID);

    CFStringRef console_uid = NULL, out_uid = NULL, out_name = NULL;
    AudioObjectID existing = 0, out_dev = 0;
    char want[256] = "";
    if (strcmp(argv[1], "--list")) snprintf(want, sizeof want, "%s", argv[1]);

    for (UInt32 i = 0; i < n; i++) {
        CFStringRef uid = cfprop(ids[i], kAudioDevicePropertyDeviceUID);
        CFStringRef name = cfprop(ids[i], kAudioObjectPropertyName);
        char nm[256] = "?";
        if (name) CFStringGetCString(name, nm, sizeof nm, kCFStringEncodingUTF8);
        UInt32 ins = channels(ids[i], kAudioObjectPropertyScopeInput);
        UInt32 outs = channels(ids[i], kAudioObjectPropertyScopeOutput);
        if (!want[0]) printf("  %-40s in %2u  out %2u\n", nm, ins, outs);
        if (uid && CFEqual(uid, AGG_UID)) existing = ids[i];
        else if (uid && CFStringHasPrefix(uid, CFSTR("slfw:"))) console_uid = CFRetain(uid);
        else if (want[0] && outs && !out_uid && strcasestr(nm, want)) {
            out_uid = CFRetain(uid); out_name = CFRetain(name); out_dev = ids[i];
        }
        if (uid) CFRelease(uid);
        if (name) CFRelease(name);
    }
    if (!want[0]) return 0;
    if (!console_uid) { fprintf(stderr, "the console driver is not visible to Core Audio\n"); return 1; }
    if (!out_uid) { fprintf(stderr, "no output device matching \"%s\" (try --list)\n", want); return 1; }

    if (existing) {
        OSStatus r = AudioHardwareDestroyAggregateDevice(existing);
        printf("replaced the previous aggregate (%d)\n", (int)r);
    }

    /* Match the output to the console's rate first; an aggregate's sub-devices
     * must share a nominal rate. */
    Float64 rate = 44100;
    AudioObjectID console = 0;
    for (UInt32 i = 0; i < n; i++) {
        CFStringRef uid = cfprop(ids[i], kAudioDevicePropertyDeviceUID);
        if (uid && CFEqual(uid, console_uid)) console = ids[i];
        if (uid) CFRelease(uid);
    }
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    sz = sizeof rate;
    AudioObjectGetPropertyData(console, &ra, 0, NULL, &sz, &rate);
    OSStatus rr = AudioObjectSetPropertyData(out_dev, &ra, 0, NULL, sizeof rate, &rate);
    printf("output device set to %.0f Hz (%d)\n", rate, (int)rr);

    CFMutableDictionaryRef sub_console = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub_console, CFSTR(kAudioSubDeviceUIDKey), console_uid);
    CFMutableDictionaryRef sub_out = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub_out, CFSTR(kAudioSubDeviceUIDKey), out_uid);
    CFDictionarySetValue(sub_out, CFSTR(kAudioSubDeviceDriftCompensationKey), kCFBooleanTrue);
    CFTypeRef subs[2] = { sub_console, sub_out };
    CFArrayRef list = CFArrayCreate(NULL, subs, 2, &kCFTypeArrayCallBacks);

    char outn[200] = "";
    CFStringGetCString(out_name, outn, sizeof outn, kCFStringEncodingUTF8);
    char aggn[256];
    snprintf(aggn, sizeof aggn, "StudioLive + %s", outn);
    CFStringRef agg_name = CFStringCreateWithCString(NULL, aggn, kCFStringEncodingUTF8);

    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), agg_name);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), AGG_UID);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), list);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), console_uid);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), kCFBooleanFalse);
    CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsStackedKey), kCFBooleanFalse);

    AudioObjectID agg = 0;
    OSStatus r = AudioHardwareCreateAggregateDevice(desc, &agg);
    if (r != noErr) { fprintf(stderr, "AudioHardwareCreateAggregateDevice failed: %d\n", (int)r); return 1; }
    usleep(500000);   /* the aggregate finishes building its sub-device list asynchronously */
    printf("created \"%s\" (device %u): %u inputs, %u outputs, clock from the console\n",
           aggn, agg, channels(agg, kAudioObjectPropertyScopeInput), channels(agg, kAudioObjectPropertyScopeOutput));
    return 0;
}
