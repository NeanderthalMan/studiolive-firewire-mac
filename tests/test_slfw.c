// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * src/slfw.c without a console: packets are built by hand in the DCL buffer and
 * fed to process_slot, arrival times to clock_observe, and the ring is read
 * back through slfw_read exactly as the plug-in's I/O thread reads it.
 */
#include "../src/slfw.c"
#include "check.h"

#define CH 4

static slfw *fake(unsigned channels, unsigned rate)
{
    slfw *s = calloc(1, sizeof *s);
    s->info.channels = channels;
    s->info.sample_rate = rate;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    s->nom_tpf = (1e9 / rate) * (double)tb.denom / (double)tb.numer;
    s->ring = calloc((size_t)RING_FRAMES * channels, sizeof(int32_t));
    s->pktbuf = calloc(NPKT, PKTSZ);
    clock_reset(s);
    return s;
}

static void unfake(slfw *s) { free(s->ring); free(s->pktbuf); free(s); }

/* One IEC 61883-6 packet as OHCI leaves it in the slot: the isoch header in
 * host order, then a big-endian CIP header and data blocks. Sample c of block
 * b is sample(b, c), with a label byte the console leaves at 0x00. */
static int32_t sample(unsigned b, unsigned c) { return (int32_t)((b * 1000 + c * 10 + 1) & 0x7fffff); }

static void put_packet(slfw *s, unsigned slot, unsigned dbs, unsigned dbc, unsigned blocks, unsigned label)
{
    UInt32 *q = (UInt32 *)(s->pktbuf + (size_t)slot * PKTSZ);
    unsigned len = 8 + blocks * dbs * 4;
    q[0] = (len << 16) | (0xa << 4);
    q[1] = OSSwapHostToBigInt32((dbs << 16) | (dbc & 0xff));
    q[2] = OSSwapHostToBigInt32((0x10u << 24) | (0x02u << 16));    /* FDF: 48 kHz, as this console lies */
    for (unsigned b = 0; b < blocks; b++)
        for (unsigned c = 0; c < dbs; c++)
            q[3 + b * dbs + c] = OSSwapHostToBigInt32((label << 24) | (uint32_t)sample(b, c));
}

static void test_depacketise(void)
{
    TEST("depacketise: samples, label byte, channel count");
    slfw *s = fake(CH, 44100);
    put_packet(s, 0, CH + 2, 0, 8, 0x00);          /* two extra slots in each block: MIDI, say */
    process_slot(s, 0);
    CHECK(atomic_load(&s->wpos) == 8, "wpos %llu", atomic_load(&s->wpos));
    CHECK(s->data_packets == 1 && s->bad_packets == 0, "data %llu bad %llu", s->data_packets, s->bad_packets);
    for (unsigned b = 0; b < 8; b++)
        for (unsigned c = 0; c < CH; c++)
            CHECK(s->ring[b * CH + c] == sample(b, c) << 8, "block %u ch %u: %d", b, c, s->ring[b * CH + c]);

    /* IEC 61883-6 says the label is 0x40; the console sends 0x00. Either way it
     * is shifted out and never reaches the audio. */
    put_packet(s, 1, CH, 8, 8, 0x40);
    process_slot(s, 1);
    CHECK(s->ring[8 * CH] == sample(0, 0) << 8, "label leaked into sample: %d", s->ring[8 * CH]);

    /* A negative 24-bit sample stays negative once left-justified. */
    UInt32 *q = (UInt32 *)(s->pktbuf + 2 * PKTSZ);
    put_packet(s, 2, CH, 16, 1, 0x40);
    q[3] = OSSwapHostToBigInt32(0x40800000);       /* -2^23, full-scale negative */
    process_slot(s, 2);
    CHECK(s->ring[16 * CH] == INT32_MIN, "full-scale negative: %d", s->ring[16 * CH]);
    unfake(s);

    TEST("depacketise: a device with fewer slots than channels zero-fills the rest");
    s = fake(CH, 44100);
    put_packet(s, 0, 2, 0, 1, 0);
    process_slot(s, 0);
    CHECK(s->ring[2] == 0 && s->ring[3] == 0, "unfilled channels %d %d", s->ring[2], s->ring[3]);
    unfake(s);
}

static void test_dbc(void)
{
    TEST("DBC: empty packets carry the next block's DBC and cost nothing");
    slfw *s = fake(CH, 44100);
    put_packet(s, 0, CH, 0, 8, 0);
    put_packet(s, 1, CH, 8, 0, 0);                 /* empty: DBC of the next data block */
    put_packet(s, 2, CH, 8, 8, 0);
    for (unsigned i = 0; i < 3; i++) process_slot(s, i);
    CHECK(s->dbc_breaks == 0 && s->frames_lost == 0, "breaks %llu lost %llu", s->dbc_breaks, s->frames_lost);
    CHECK(atomic_load(&s->wpos) == 16, "wpos %llu", atomic_load(&s->wpos));
    CHECK(s->packets == 3 && s->data_packets == 2, "packets %llu data %llu", s->packets, s->data_packets);

    TEST("DBC: a lost packet is counted and filled with silence, keeping the timeline");
    put_packet(s, 3, CH, 24, 8, 0);                /* 16..23 never arrived */
    process_slot(s, 3);
    CHECK(s->dbc_breaks == 1 && s->frames_lost == 8, "breaks %llu lost %llu", s->dbc_breaks, s->frames_lost);
    CHECK(atomic_load(&s->wpos) == 32, "wpos %llu", atomic_load(&s->wpos));
    for (unsigned f = 16; f < 24; f++) CHECK(s->ring[f * CH] == 0, "gap frame %u: %d", f, s->ring[f * CH]);
    CHECK(s->ring[24 * CH] == sample(0, 0) << 8, "after the gap: %d", s->ring[24 * CH]);

    TEST("DBC: wraps at 256 without a break");
    s->expect_dbc = 252;
    put_packet(s, 4, CH, 252, 8, 0);
    put_packet(s, 5, CH, 4, 8, 0);
    process_slot(s, 4); process_slot(s, 5);
    CHECK(s->dbc_breaks == 1, "breaks %llu", s->dbc_breaks);
    unfake(s);

    TEST("bad packets are counted and change nothing");
    s = fake(CH, 44100);
    put_packet(s, 0, CH, 0, 8, 0);
    ((UInt32 *)s->pktbuf)[0] = (40u << 16) | (0x1 << 4);            /* not an isoch data tcode */
    process_slot(s, 0);
    put_packet(s, 1, CH, 0, 8, 0);
    ((UInt32 *)(s->pktbuf + PKTSZ))[2] = OSSwapHostToBigInt32(0x01u << 24);   /* not AM824 */
    process_slot(s, 1);
    CHECK(s->bad_packets == 2 && atomic_load(&s->wpos) == 0, "bad %llu wpos %llu", s->bad_packets, atomic_load(&s->wpos));
    unfake(s);
}

static void fill(slfw *s, uint64_t upto)
{
    for (uint64_t f = atomic_load(&s->wpos); f < upto; f++)
        for (unsigned c = 0; c < s->info.channels; c++)
            s->ring[(f % RING_FRAMES) * s->info.channels + c] = (int32_t)(f * 16 + c + 1);
    atomic_store(&s->wpos, upto);
}

static void test_read(void)
{
    int32_t *buf = calloc((size_t)512 * CH, sizeof(int32_t));
    slfw *s = fake(CH, 44100);
    fill(s, 10000);

    TEST("read: frames already written come back exactly");
    unsigned real = slfw_read(s, 9000, buf, 256);
    CHECK(real == 256, "real %u", real);
    CHECK(buf[0] == 9000 * 16 + 1 && buf[255 * CH + 3] == 9255 * 16 + 4, "%d %d", buf[0], buf[255 * CH + 3]);

    TEST("read: frames not yet written are zeros, and only they are");
    real = slfw_read(s, 9900, buf, 256);
    CHECK(real == 100, "real %u", real);
    CHECK(buf[99 * CH] == 9999 * 16 + 1, "last real frame %d", buf[99 * CH]);
    CHECK(buf[100 * CH] == 0 && buf[255 * CH + 3] == 0, "future frames %d %d", buf[100 * CH], buf[255 * CH + 3]);

    TEST("read: frames the writer may already be overwriting are zeros");
    fill(s, (uint64_t)RING_FRAMES + 20000);
    /* strictly inside: frame w - RING + MARGIN is the writer's next victim */
    uint64_t oldest = atomic_load(&s->wpos) - RING_FRAMES + READ_MARGIN + 1;
    real = slfw_read(s, oldest - 10, buf, 256);
    CHECK(real == 246, "real %u", real);
    CHECK(buf[9 * CH] == 0 && buf[10 * CH] == (int32_t)(oldest * 16 + 1), "%d %d", buf[9 * CH], buf[10 * CH]);

    TEST("read: the ring wraps");
    uint64_t w = atomic_load(&s->wpos);
    real = slfw_read(s, w - 300, buf, 256);
    CHECK(real == 256 && buf[0] == (int32_t)((w - 300) * 16 + 1), "real %u first %d", real, buf[0]);

    unfake(s);
    free(buf);
}

/* Arrivals every 32 packets, as the DCL program delivers them: 176.4 frames at
 * 44.1 kHz, rounded to whole 8-frame blocks as the console sends them. Each is
 * late by jitter in [0, max_late) ticks — never early, which is what the
 * minimum-envelope fit relies on. */
static double run_clock(slfw *s, double true_tpf, double max_late, double seconds, unsigned seed)
{
    srand(seed);
    double t0 = 1e12, frames = 0;
    unsigned groups = (unsigned)(seconds * 8000 / GROUP);
    for (unsigned g = 0; g < groups; g++) {
        frames += s->info.sample_rate * (double)GROUP / 8000.0;
        uint64_t f = (uint64_t)(frames / 8) * 8;
        double late = max_late * rand() / ((double)RAND_MAX + 1);
        clock_observe(s, f, (uint64_t)(t0 + f * true_tpf + late));
    }
    return t0;
}

static void test_clock(void)
{
    double tpf, bh; uint64_t bf; uint32_t seed;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ms = 1e6 * (double)tb.denom / (double)tb.numer;          /* ticks per millisecond */

    TEST("clock: nothing to report before the first packets");
    slfw *s = fake(CH, 44100);
    CHECK(!slfw_clock(s, &tpf, &bf, &bh, &seed), "valid with no arrivals");

    TEST("clock: the console's rate is recovered through 3 ms of call-back jitter");
    double true_tpf = s->nom_tpf * (1 + 30e-6);                    /* the console runs 30 ppm slow, as measured */
    double t0 = run_clock(s, true_tpf, 3 * ms, 40, 1);
    CHECK(slfw_clock(s, &tpf, &bf, &bh, &seed), "no clock after 40 s");
    CHECK_NEAR((tpf / true_tpf - 1) * 1e6, 0, 2, "rate error, ppm");
    /* The line hugs the earliest arrivals: host_time(f) is when f is first
     * available, not the average — never more than the jitter late. */
    uint64_t f = 1000000;
    double at = bh + ((double)f - (double)bf) * tpf;
    double truth = t0 + f * true_tpf;
    CHECK(at >= truth - 0.2 * ms && at <= truth + 1.0 * ms, "frame %llu placed %.3f ms from its earliest arrival",
          f, (at - truth) / ms);
    unfake(s);

    TEST("clock: a fit percent away from nominal is clamped, not trusted");
    s = fake(CH, 44100);
    run_clock(s, s->nom_tpf * 1.01, 0, 20, 2);
    CHECK(slfw_clock(s, &tpf, &bf, &bh, &seed), "no clock");
    CHECK(tpf == s->nom_tpf, "tpf %.6f, nominal %.6f", tpf, s->nom_tpf);

    TEST("clock: a reset gives a new seed and no clock until packets return");
    uint32_t before = seed;
    clock_reset(s);
    CHECK(!slfw_clock(s, &tpf, &bf, &bh, &seed), "still valid after reset");
    run_clock(s, s->nom_tpf, 0, 1, 3);
    CHECK(slfw_clock(s, &tpf, &bf, &bh, &seed) && seed != before, "seed %u, was %u", seed, before);
    unfake(s);
}

static void test_callback_gap(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t ms = (uint64_t)(1e6 * (double)tb.denom / (double)tb.numer);
    slfw_stats st;

    TEST("call-back gap: the worst wait between call-backs is kept, in µs");
    slfw *s = fake(CH, 44100);
    slfw_get_stats(s, &st);
    CHECK(st.worst_callback_gap_us == 0, "before any call-back: %u", st.worst_callback_gap_us);
    uint64_t t = 1000 * ms;
    note_callback(s, t);
    slfw_get_stats(s, &st);
    CHECK(st.worst_callback_gap_us == 0, "one call-back is no gap: %u", st.worst_callback_gap_us);
    note_callback(s, t += 4 * ms);
    note_callback(s, t += 25 * ms);                /* the late one */
    note_callback(s, t += 4 * ms);
    slfw_get_stats(s, &st);
    CHECK_NEAR(st.worst_callback_gap_us, 25000, 1, "worst gap, µs");

    TEST("call-back gap: a clock that does not advance is ignored, not a huge gap");
    note_callback(s, t - 10 * ms);
    note_callback(s, t - 10 * ms);
    slfw_get_stats(s, &st);
    CHECK_NEAR(st.worst_callback_gap_us, 25000, 1, "worst gap, µs");
    unfake(s);
}

int main(void)
{
    test_depacketise();
    test_dbc();
    test_read();
    test_clock();
    test_callback_gap();
    return check_done("slfw");
}
