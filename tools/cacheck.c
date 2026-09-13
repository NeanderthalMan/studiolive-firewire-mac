// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cacheck — record from the installed driver THROUGH Core Audio.
 *
 *   cacheck [seconds]
 *
 * halcheck drives the plug-in directly, in its own process, outside any
 * sandbox. This is the other half: an ordinary Core Audio client that finds the
 * device the way Studio One does and records from it, so the audio has come
 * through coreaudiod and the sandboxed driver service. If the sandbox refused
 * IOFireWireUserClient the device will not be listed at all, and the unified
 * log says why:
 *   log show --last 5m --predicate 'subsystem == "n12n.presonus-adapter"'
 */
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXCH 64

static _Atomic uint64_t g_cycles, g_frames, g_gaps;
static double g_peak[MAXCH];
static Float64 g_next = -1;
static UInt32 g_ch;

static OSStatus ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in,
                       const AudioTimeStamp *inTime, AudioBufferList *out, const AudioTimeStamp *outTime,
                       void *ctx)
{
    if (!in || in->mNumberBuffers == 0) return noErr;
    const AudioBuffer *b = &in->mBuffers[0];
    UInt32 ch = b->mNumberChannels, frames = b->mDataByteSize / (sizeof(Float32) * (ch ? ch : 1));
    const Float32 *d = b->mData;
    for (UInt32 i = 0; i < frames * ch; i++) {
        double v = fabs(d[i]);
        if (i % ch < MAXCH && v > g_peak[i % ch]) g_peak[i % ch] = v;
    }
    if (g_next >= 0 && fabs(inTime->mSampleTime - g_next) > 0.5) atomic_fetch_add(&g_gaps, 1);
    g_next = inTime->mSampleTime + frames;
    atomic_fetch_add(&g_cycles, 1);
    atomic_fetch_add(&g_frames, frames);
    return noErr;
}

static AudioObjectID find_device(char *name, size_t namelen)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID *ids = malloc(sz);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    AudioObjectID found = kAudioObjectUnknown;
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID) && !found; i++) {
        CFStringRef uid = NULL; UInt32 usz = sizeof uid;
        AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &usz, &uid) == noErr && uid) {
            if (CFStringHasPrefix(uid, CFSTR("slfw:")) && !CFEqual(uid, CFSTR("slfw:aggregate"))) {
                found = ids[i];
                CFStringRef nm = NULL; UInt32 nsz = sizeof nm;
                AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                                  kAudioObjectPropertyElementMain };
                if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &nsz, &nm) == noErr && nm) {
                    CFStringGetCString(nm, name, namelen, kCFStringEncodingUTF8);
                    CFRelease(nm);
                }
            }
            CFRelease(uid);
        }
    }
    free(ids);
    return found;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 10;
    char name[256] = "?";
    AudioObjectID dev = find_device(name, sizeof name);
    if (!dev) {
        printf("FAIL: no slfw device is visible to Core Audio.\n"
               "  Either the driver is not installed, the console is off, or the sandbox refused\n"
               "  the FireWire user client. Check:\n"
               "  log show --last 5m --predicate 'subsystem == \"n12n.presonus-adapter\"'\n");
        return 1;
    }
    Float64 rate = 0; UInt32 sz = sizeof rate;
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &rate);

    AudioObjectPropertyAddress ca = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput,
                                      kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyDataSize(dev, &ca, 0, NULL, &sz);
    AudioBufferList *bl = malloc(sz);
    AudioObjectGetPropertyData(dev, &ca, 0, NULL, &sz, bl);
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++) g_ch += bl->mBuffers[i].mNumberChannels;
    free(bl);

    char chname[8][64];
    for (UInt32 e = 1; e <= 8 && e <= g_ch; e++) {
        CFStringRef s = NULL; UInt32 ssz = sizeof s;
        AudioObjectPropertyAddress ea = { kAudioObjectPropertyElementName, kAudioObjectPropertyScopeInput, e };
        strcpy(chname[e - 1], "?");
        if (AudioObjectGetPropertyData(dev, &ea, 0, NULL, &ssz, &s) == noErr && s) {
            CFStringGetCString(s, chname[e - 1], 64, kCFStringEncodingUTF8); CFRelease(s);
        }
    }
    printf("device %u \"%s\": %.0f Hz, %u input channels (first: %s, %s ...)\n",
           dev, name, rate, g_ch, chname[0], g_ch > 1 ? chname[1] : "");

    AudioDeviceIOProcID procid = NULL;
    OSStatus r = AudioDeviceCreateIOProcID(dev, ioproc, NULL, &procid);
    if (r) { printf("FAIL: AudioDeviceCreateIOProcID %d\n", (int)r); return 1; }
    r = AudioDeviceStart(dev, procid);
    if (r) { printf("FAIL: AudioDeviceStart %d — see the unified log\n", (int)r); return 1; }
    printf("recording through Core Audio for %.0f s...\n", secs);
    usleep((useconds_t)(secs * 1e6));
    AudioDeviceStop(dev, procid);
    AudioDeviceDestroyIOProcID(dev, procid);

    uint64_t frames = atomic_load(&g_frames);
    printf("\ncycles %llu, frames %llu (%.2f s), sample-time gaps %llu\n", atomic_load(&g_cycles), frames,
           frames / rate, atomic_load(&g_gaps));
    unsigned live = 0;
    for (UInt32 c = 0; c < g_ch && c < MAXCH; c++) if (g_peak[c] > 0) live++;
    printf("channels with non-zero audio: %u of %u\n", live, g_ch);
    for (UInt32 c = 0; c < g_ch && c < MAXCH; c += (c < 3 || c >= g_ch - 2) ? 1 : g_ch) {}
    printf("peaks (dBFS):");
    for (UInt32 c = 0; c < g_ch && c < MAXCH; c++) {
        if (c % 8 == 0) printf("\n ");
        printf(" %3u:%6.1f", c + 1, 20 * log10(g_peak[c] + 1e-12));
    }
    printf("\n");
    bool ok = frames > rate * secs * 0.9 && atomic_load(&g_gaps) <= 1 && live == g_ch;
    printf("\n%s\n", ok ? "PASSED — audio is reaching Core Audio clients" : "FAILED");
    return ok ? 0 : 1;
}
