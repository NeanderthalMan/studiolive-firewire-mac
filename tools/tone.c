// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tone — play a test tone out of a named output device.
 *
 *   tone "<device name substring>" [seconds] [left|right|both]
 *
 * Separates "the output path works" from "Studio One's routing is wrong": if
 * this is audible and Studio One is not, the problem is inside the song.
 * 440 Hz at -20 dBFS, short fades so it does not click.
 */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static double g_phase, g_rate = 44100, g_total;
static uint64_t g_n;
static int g_side;   /* 0 both, 1 left, 2 right */

static OSStatus proc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in,
                     const AudioTimeStamp *it, AudioBufferList *out, const AudioTimeStamp *ot, void *c)
{
    if (!out) return noErr;
    UInt32 frames = 0;
    for (UInt32 b = 0; b < out->mNumberBuffers; b++) {
        AudioBuffer *buf = &out->mBuffers[b];
        UInt32 ch = buf->mNumberChannels ? buf->mNumberChannels : 1;
        frames = buf->mDataByteSize / (4 * ch);
        Float32 *s = buf->mData;
        double ph = g_phase;
        for (UInt32 i = 0; i < frames; i++) {
            double t = (double)(g_n + i) / g_rate, fade = 1;
            if (t < 0.05) fade = t / 0.05;
            if (t > g_total - 0.05) fade = fmax(0, (g_total - t) / 0.05);
            Float32 v = (Float32)(0.1 * fade * sin(ph));
            ph += 2 * M_PI * 440 / g_rate;
            for (UInt32 c2 = 0; c2 < ch; c2++)
                s[i * ch + c2] = (g_side == 0 || (g_side == 1 && c2 == 0) || (g_side == 2 && c2 == 1)) ? v : 0;
        }
        if (b == out->mNumberBuffers - 1) g_phase = fmod(ph, 2 * M_PI);
    }
    g_n += frames;
    return noErr;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: tone \"<device>\" [seconds] [left|right|both]\n"); return 2; }
    g_total = argc > 2 ? atof(argv[2]) : 3;
    if (argc > 3) g_side = !strcmp(argv[3], "left") ? 1 : !strcmp(argv[3], "right") ? 2 : 0;

    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &da, 0, NULL, &sz);
    AudioObjectID ids[64];
    if (sz > sizeof ids) sz = sizeof ids;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &sz, ids);
    AudioObjectID dev = 0;
    char name[128];
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID) && !dev; i++) {
        CFStringRef s = NULL; UInt32 z = sizeof s;
        AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, 0 };
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &z, &s) == noErr && s) {
            CFStringGetCString(s, name, sizeof name, kCFStringEncodingUTF8);
            CFRelease(s);
            AudioObjectPropertyAddress oa = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, 0 };
            UInt32 osz = 0;
            AudioObjectGetPropertyDataSize(ids[i], &oa, 0, NULL, &osz);
            if (osz && strcasestr(name, argv[1])) dev = ids[i];
        }
    }
    if (!dev) { fprintf(stderr, "no output device matching \"%s\"\n", argv[1]); return 1; }
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0 };
    sz = sizeof g_rate;
    AudioObjectGetPropertyData(dev, &ra, 0, NULL, &sz, &g_rate);

    AudioDeviceIOProcID id = NULL;
    if (AudioDeviceCreateIOProcID(dev, proc, NULL, &id) || AudioDeviceStart(dev, id)) {
        fprintf(stderr, "could not start output on %s\n", name); return 1;
    }
    printf("playing 440 Hz (%s) on \"%s\" for %.1f s...\n",
           g_side == 1 ? "left" : g_side == 2 ? "right" : "both sides", name, g_total);
    usleep((useconds_t)((g_total + 0.2) * 1e6));
    AudioDeviceStop(dev, id);
    AudioDeviceDestroyIOProcID(dev, id);
    return 0;
}
