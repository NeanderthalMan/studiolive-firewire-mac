// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * driver/plugin.c's real-time path without Core Audio or a console: a fake
 * slfw stream stands in for the FireWire side, and the I/O functions are called
 * the way the HAL's I/O thread calls them.
 */
#include "../src/slfw.c"
#include "../driver/plugin.c"
#include "check.h"

#define CH 2

static slfw *fake_stream(unsigned channels, unsigned rate)
{
    slfw *s = calloc(1, sizeof *s);
    s->info.channels = channels;
    s->info.sample_rate = rate;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    s->nom_tpf = (1e9 / rate) * (double)tb.denom / (double)tb.numer;
    s->ring = calloc((size_t)RING_FRAMES * channels, sizeof(int32_t));
    clock_reset(s);
    return s;
}

static void use_stream(slfw *s)
{
    gInfo.channels = s->info.channels;
    gInfo.sample_rate = s->info.sample_rate;
    atomic_store(&gFW, s);
    atomic_store(&gUnderruns, 0);
    if (!gScratch) gScratch = calloc((size_t)kMaxIOFrames * SLFW_MAX_CH, sizeof(int32_t));
}

static void test_cadence(void)
{
    TEST("cadence: frames per 8000 packets/s names the rate");
    CHECK(cadence_rate(0, 0) == 0, "no packets");
    CHECK(cadence_rate(2000, 3999) == 0, "too few packets to trust");
    CHECK(cadence_rate(44100, 8000) == 44100, "44.1 kHz");
    CHECK(cadence_rate(48000, 8000) == 48000, "48 kHz");
    CHECK(cadence_rate(96000, 8000) == 96000, "96 kHz");
    /* The start-of-stream reading from 2026-09-26: a few frames over. */
    CHECK(cadence_rate(19928, 3614) == 0, "under 4000 packets");
    CHECK(cadence_rate(59792, 10846) == 44100, "59792/10846");
    TEST("cadence: nothing near a standard rate is no answer, not a guess");
    CHECK(cadence_rate(46000, 8000) == 0, "46 kHz is 4%% from both neighbours");
}

static void test_read_input(void)
{
    slfw *s = fake_stream(CH, 44100);
    use_stream(s);
    for (uint64_t f = 0; f < 8192; f++) {
        s->ring[f * CH] = (int32_t)(f << 16);
        s->ring[f * CH + 1] = -(int32_t)(f << 16);
    }
    atomic_store(&s->wpos, 8192);

    Float32 out[512 * CH];
    AudioServerPlugInIOCycleInfo cycle = { 0 };

    TEST("I/O: ReadInput converts the ring to float at 2^-31");
    cycle.mInputTime.mSampleTime = 4096;
    DoIOOperation(NULL, kObj_Device, kObj_Stream, 0, kAudioServerPlugInIOOperationReadInput, 256, &cycle, out, NULL);
    CHECK_NEAR(out[0], (4096 << 16) / 2147483648.0, 1e-9, "first sample");
    CHECK_NEAR(out[255 * CH + 1], -(double)(4351 << 16) / 2147483648.0, 1e-9, "last sample, right");
    CHECK(atomic_load(&gUnderruns) == 0, "underruns %llu", atomic_load(&gUnderruns));

    TEST("I/O: reading past the writer counts an underrun and zero-fills");
    cycle.mInputTime.mSampleTime = 8192 - 253;               /* 3 frames short, as on 2026-09-26 */
    DoIOOperation(NULL, kObj_Device, kObj_Stream, 0, kAudioServerPlugInIOOperationReadInput, 256, &cycle, out, NULL);
    CHECK(atomic_load(&gUnderruns) == 3, "underruns %llu", atomic_load(&gUnderruns));
    CHECK(out[253 * CH] == 0 && out[255 * CH + 1] == 0, "zero fill %g %g", out[253 * CH], out[255 * CH + 1]);
    CHECK(atomic_load(&gShortPos) == 8192 - 253 && atomic_load(&gShortFrames) == 256 &&
          atomic_load(&gShortWriter) == 8192, "short read recorded as [%llu +%llu) writer %llu",
          atomic_load(&gShortPos), atomic_load(&gShortFrames), atomic_load(&gShortWriter));

    TEST("I/O: WriteMix is the sink — accepted and never read");
    CHECK(DoIOOperation(NULL, kObj_Device, kObj_StreamOut, 0, kAudioServerPlugInIOOperationWriteMix, 256, &cycle,
                        NULL, NULL) == noErr, "WriteMix with a NULL buffer");

    TEST("I/O: no stream means silence, not a crash");
    atomic_store(&gFW, NULL);
    out[0] = 1;
    DoIOOperation(NULL, kObj_Device, kObj_Stream, 0, kAudioServerPlugInIOOperationReadInput, 256, &cycle, out, NULL);
    CHECK(out[0] == 0, "out[0] %g", out[0]);
    free(s->ring); free(s);
}

static void test_zero_timestamps(void)
{
    slfw *s = fake_stream(CH, 44100);
    use_stream(s);
    atomic_store(&gHaveClock, true);
    double tpf = s->nom_tpf;
    double now = (double)mach_absolute_time();
    Float64 st; UInt64 ht, seed;

    TEST("zero timestamps: on a period boundary, where the fit puts it");
    gZSample = -1;
    double base_host = now - 100000 * tpf;                   /* frame 100000 is now */
    clock_publish(s, tpf, 0, base_host);
    GetZeroTimeStamp(NULL, kObj_Device, 0, &st, &ht, &seed);
    CHECK(st == 98304, "sample time %.0f, want 6 x 16384", st);
    CHECK_NEAR((double)ht, base_host + 98304 * tpf, 2, "host time");

    TEST("zero timestamps: a fit that jumps is walked in at no more than 200 ppm");
    /* No real time passes in a test, so reaching the next period means moving
     * the fit a whole period earlier: to the steer that is a phase error of
     * nearly -16384 frames, and it must move by the 200 ppm limit and no more. */
    Float64 st0 = st; UInt64 ht0 = ht;
    clock_publish(s, tpf, 0, base_host - 16384 * tpf);
    GetZeroTimeStamp(NULL, kObj_Device, 0, &st, &ht, &seed);
    CHECK(st == st0 + 16384, "sample time %.0f, want %.0f", st, st0 + 16384);
    double pred = (double)ht0 + 16384 * tpf, lim = 16384 * tpf * 200e-6;
    CHECK_NEAR((double)ht - pred, -lim, 2, "steer, ticks");
    CHECK((double)ht > (double)ht0, "host time went backwards");

    TEST("zero timestamps: a fit that steps back never moves time backwards");
    Float64 st1 = st; UInt64 ht1 = ht;
    clock_publish(s, tpf, 0, base_host + 16384 * tpf);        /* the fit now says we are a period earlier */
    GetZeroTimeStamp(NULL, kObj_Device, 0, &st, &ht, &seed);
    CHECK(st == st1 && ht == ht1, "sample %.0f host %llu, latched %.0f %llu", st, ht, st1, ht1);

    TEST("zero timestamps: a new stream (seed) re-latches at once");
    UInt64 old_seed = seed;
    clock_reset(s);
    clock_publish(s, tpf, 0, now - 50000 * tpf);
    GetZeroTimeStamp(NULL, kObj_Device, 0, &st, &ht, &seed);
    CHECK(seed != old_seed && st == 49152, "seed %llu (was %llu), sample %.0f", seed, old_seed, st);

    atomic_store(&gFW, NULL);
    free(s->ring); free(s);
}

static SInt64 num(CFDictionaryRef d, CFStringRef k)
{
    SInt64 v = -1;
    CFNumberRef n = CFDictionaryGetValue(d, k);
    if (n) CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

static void test_status_property(void)
{
    AudioObjectPropertyAddress a = { kAudioObjectPropertyCustomPropertyInfoList, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0;
    atomic_store(&gPresent, true);
    snprintf(gInfo.nickname, sizeof gInfo.nickname, "StudioLive");

    TEST("status property: listed as a custom property holding a property list");
    AudioServerPlugInCustomPropertyInfo info = { 0 };
    CHECK(prop(kObj_Device, &a, 0, NULL, sizeof info, &sz, &info) == noErr && sz == sizeof info, "sz %u", sz);
    CHECK(info.mSelector == kSLFWPropertyStatus && info.mPropertyDataType == kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
          "selector %08x type %08x", info.mSelector, info.mPropertyDataType);

    TEST("status property: idle — identity and counters, no stream keys");
    a.mSelector = kSLFWPropertyStatus;
    gInfo.channels = 40; gInfo.sample_rate = 44100;
    atomic_store(&gFW, NULL);
    atomic_store(&gUnderruns, 3);
    CFDictionaryRef d = NULL;
    CHECK(prop(kObj_Device, &a, 0, NULL, sizeof d, &sz, &d) == noErr && d, "no dictionary");
    if (d) {
        CHECK(num(d, CFSTR("channels")) == 40 && num(d, CFSTR("sample rate")) == 44100, "identity");
        CHECK(num(d, CFSTR("read underruns")) == 3, "underruns %lld", num(d, CFSTR("read underruns")));
        CHECK(!CFDictionaryContainsKey(d, CFSTR("frames")), "frames without a stream");
        CHECK(CFEqual(CFDictionaryGetValue(d, CFSTR("console")), CFSTR("StudioLive")), "console name");
        CFRelease(d);
    }

    TEST("status property: streaming — the stream's own counters");
    slfw *s = fake_stream(CH, 44100);
    atomic_store(&s->wpos, 123456);
    atomic_store(&s->frames_lost, 8);
    use_stream(s);
    d = NULL;
    prop(kObj_Device, &a, 0, NULL, sizeof d, &sz, &d);
    CHECK(d && num(d, CFSTR("frames")) == 123456 && num(d, CFSTR("frames lost")) == 8, "stream counters");
    CHECK(atomic_load(&gInIO) == 0, "stream left pinned: %d", atomic_load(&gInIO));
    if (d) CFRelease(d);
    atomic_store(&gFW, NULL);
    atomic_store(&gPresent, false);
    free(s->ring); free(s);
}

static void *realtime_probe(void *arg)
{
    make_realtime();
    thread_time_constraint_policy_data_t p = { 0 };
    mach_msg_type_number_t n = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t dflt = false;              /* true would ask for the default policy, not this thread's */
    kern_return_t r = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                        (thread_policy_t)&p, &n, &dflt);
    *(bool *)arg = r == KERN_SUCCESS && !dflt && p.period > 0 && p.computation < p.constraint;
    return NULL;
}

static void test_realtime(void)
{
    TEST("FireWire thread: time-constrained, so a busy Mac cannot starve the call-backs");
    bool ok = false;
    pthread_t th;
    pthread_create(&th, NULL, realtime_probe, &ok);
    pthread_join(th, NULL);
    CHECK(ok, "thread_policy_get does not show the time-constraint policy");
}

int main(void)
{
    gLog = os_log_create("n12n.presonus-adapter", "test");
    test_cadence();
    test_read_input();
    test_zero_timestamps();
    test_realtime();
    test_status_property();
    return check_done("plugin");
}
