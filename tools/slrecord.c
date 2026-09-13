// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * slrecord — record every channel of a StudioLive AI console to one WAV.
 *
 *   slrecord [-s seconds] [-o file.wav]
 *
 * Also the physical acceptance test for the depacketiser: put a signal into one input,
 * and the level table shows it in exactly that channel.
 */
#include "../src/slfw.h"

#include <mach/mach_time.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logmsg(const char *m) { fprintf(stderr, "  [slfw] %s\n", m); }

static slfw     *g_s;
static FILE     *g_f;
static uint64_t  g_rpos, g_data_bytes, g_real;
static unsigned  g_ch;
static int32_t  *g_buf;
static double    g_peak[SLFW_MAX_CH], g_sumsq[SLFW_MAX_CH];
static volatile sig_atomic_t g_quit;

#define CHUNK 4096

static void on_sig(int n) { (void)n; g_quit = 1; }
static void at_exit(void) { if (g_s) slfw_stop(g_s); }

static void put_u32(FILE *f, uint32_t v) { fputc(v, f); fputc(v >> 8, f); fputc(v >> 16, f); fputc(v >> 24, f); }
static void put_u16(FILE *f, uint16_t v) { fputc(v, f); fputc(v >> 8, f); }

static void wav_header(FILE *f, unsigned ch, unsigned rate, uint32_t data_bytes)
{
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); put_u32(f, 4 + 8 + 40 + 8 + data_bytes);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put_u32(f, 40);
    put_u16(f, 0xFFFE); put_u16(f, ch); put_u32(f, rate);
    put_u32(f, rate * ch * 3); put_u16(f, ch * 3); put_u16(f, 24);
    put_u16(f, 22); put_u16(f, 24); put_u32(f, 0);
    static const uint8_t pcm[16] = { 1,0,0,0, 0,0, 0x10,0, 0x80,0,0,0xaa,0,0x38,0x9b,0x71 };
    fwrite(pcm, 1, 16, f);
    fwrite("data", 1, 4, f); put_u32(f, data_bytes);
}

static void drain(void)
{
    uint64_t w = slfw_frames(g_s);
    while (g_rpos < w) {
        unsigned n = (w - g_rpos) > CHUNK ? CHUNK : (unsigned)(w - g_rpos);
        g_real += slfw_read(g_s, g_rpos, g_buf, n);
        static uint8_t out[CHUNK * SLFW_MAX_CH * 3];
        size_t k = 0;
        for (unsigned i = 0; i < n; i++)
            for (unsigned c = 0; c < g_ch; c++) {
                int32_t v = g_buf[i * g_ch + c];
                out[k++] = (v >> 8) & 0xff; out[k++] = (v >> 16) & 0xff; out[k++] = (v >> 24) & 0xff;
                double x = v / 2147483648.0;
                if (fabs(x) > g_peak[c]) g_peak[c] = fabs(x);
                g_sumsq[c] += x * x;
            }
        fwrite(out, 1, k, g_f);
        g_data_bytes += k;
        g_rpos += n;
    }
}

static void tick(CFRunLoopTimerRef t, void *info)
{
    (void)t; (void)info;
    drain();
    slfw_stats st; slfw_get_stats(g_s, &st);
    if (g_quit || st.force_stopped) CFRunLoopStop(CFRunLoopGetCurrent());
}

static double db(double x) { return x > 0 ? 20 * log10(x) : -INFINITY; }

int main(int argc, char **argv)
{
    double secs = 10;
    const char *path = "recording.wav";
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "-s")) secs = atof(argv[++i]);
        else if (!strcmp(argv[i], "-o")) path = argv[++i];
    }

    io_service_t svc = slfw_find_service(0);
    if (!svc) { fprintf(stderr, "No PreSonus FireWire device on the bus.\n"); return 1; }
    char err[256];
    g_s = slfw_open(svc, CFRunLoopGetCurrent(), logmsg, err, sizeof err);
    IOObjectRelease(svc);
    if (!g_s) { fprintf(stderr, "open failed: %s\n", err); return 1; }
    atexit(at_exit);
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    const slfw_info *in = slfw_get_info(g_s);
    g_ch = in->channels;
    g_buf = malloc((size_t)CHUNK * g_ch * sizeof(int32_t));
    g_f = fopen(path, "wb");
    if (!g_f || !g_buf) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    wav_header(g_f, g_ch, in->sample_rate, 0);

    printf("%s — %u channels at %u Hz -> %s, %.0f s\n", in->nickname, g_ch, in->sample_rate, path, secs);
    if (slfw_start(g_s) != 0) { fprintf(stderr, "start failed\n"); return 1; }

    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.05, 0.05, 0, 0, tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, secs, false);
    slfw_stop(g_s);
    drain();

    uint32_t bytes = g_data_bytes > 0xFFFFFFF0ULL ? 0xFFFFFFF0u : (uint32_t)g_data_bytes;
    wav_header(g_f, g_ch, in->sample_rate, bytes);
    fclose(g_f);

    slfw_stats st; slfw_get_stats(g_s, &st);
    double tpf, bh; uint64_t bf; uint32_t seed;
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    double measured = 0;
    if (slfw_clock(g_s, &tpf, &bf, &bh, &seed))
        measured = 1e9 / (tpf * (double)tb.numer / (double)tb.denom);

    printf("\n%-7s %9s %9s\n", "channel", "peak dBFS", "rms dBFS");
    for (unsigned c = 0; c < g_ch; c++) {
        double rms = g_rpos ? sqrt(g_sumsq[c] / g_rpos) : 0;
        printf("%-7s %9.1f %9.1f%s\n", in->names[c], db(g_peak[c]), db(rms),
               db(g_peak[c]) > -40 ? "   <- signal" : "");
    }
    printf("\nframes %llu (%.2f s at %u Hz), %llu real\n", g_rpos, (double)g_rpos / in->sample_rate,
           in->sample_rate, g_real);
    printf("packets %llu (%llu data), bad %llu, callbacks %llu\n",
           st.packets, st.data_packets, st.bad_packets, st.callbacks);
    printf("DBC breaks %llu, frames lost %llu%s\n", st.dbc_breaks, st.frames_lost,
           st.force_stopped ? ", FORCE-STOPPED" : "");
    if (measured) printf("clock: %.3f Hz measured against the Mac (%+.1f ppm)\n",
                         measured, (measured / in->sample_rate - 1) * 1e6);
    if (g_data_bytes > 0xFFFFFFF0ULL) printf("WARNING: over 4 GB; header truncated\n");
    return 0;
}
