// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * slfw — capture from a StudioLive AI console over FireWire, from user space.
 *
 * Everything here was proven on hardware first, in spike/fwprobe.c and
 * spike/isoprobe.c; see spike/README.md for the evidence behind each choice.
 * This is the shared core used by the command-line recorder and the Core Audio
 * plug-in, so it owns the device, the DICE registers, the DCL program, the
 * depacketiser, the frame ring and the clock estimate — and nothing about
 * files or Core Audio.
 *
 * Threading: slfw_open, slfw_start and slfw_stop are called from one control
 * thread. The DCL call-backs run on the CFRunLoop passed to slfw_open and are
 * the only writer. slfw_frames, slfw_read, slfw_clock and slfw_get_stats are
 * lock-free and safe from a real-time thread.
 *
 * One console per process: DCL CallProcs carry a single data word, which is
 * spent on the ring slot index, so the context is process-global.
 */
#ifndef SLFW_H
#define SLFW_H

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdint.h>

#define SLFW_MAX_CH    64
#define SLFW_NAME_LEN  32

typedef void (*slfw_log_fn)(const char *msg);

typedef struct {
    uint64_t guid;
    unsigned channels;                       /* tx[0] audio channels     */
    unsigned sample_rate;                    /* GLOBAL_STATUS at open, unless overridden */
    char     nickname[65];
    char     names[SLFW_MAX_CH][SLFW_NAME_LEN];
} slfw_info;

typedef struct {
    uint64_t callbacks, packets, data_packets, bad_packets;
    uint64_t frames, dbc_breaks, frames_lost;
    bool     running, force_stopped;
} slfw_stats;

typedef struct slfw slfw;

/* First PreSonus device on the bus when guid is 0. Caller releases. */
io_service_t slfw_find_service(uint64_t guid);

slfw            *slfw_open(io_service_t svc, CFRunLoopRef runloop,
                           slfw_log_fn log, char *err, size_t errlen);
const slfw_info *slfw_get_info(slfw *s);
int              slfw_start(slfw *s);        /* 0 on success             */
void             slfw_stop(slfw *s);         /* always leaves it quiet   */
void             slfw_close(slfw *s);

/* Frames published so far on the current stream's timeline (starts at 0). */
uint64_t slfw_frames(slfw *s);

/* Copy frames [pos, pos+n) as interleaved int32 (24-bit, left-justified).
 * Frames not yet received, or already overwritten, are zero-filled.
 * Returns how many frames were real. */
unsigned slfw_read(slfw *s, uint64_t pos, int32_t *dst, unsigned nframes);

/* host_time(f) = base_host + (f - base_frame) * ticks_per_frame, in
 * mach_absolute_time ticks, fitted to the earliest arrivals. False until the
 * first packets have arrived. seed changes whenever the timeline restarts. */
bool slfw_clock(slfw *s, double *ticks_per_frame, uint64_t *base_frame,
                double *base_host, uint32_t *seed);

void slfw_get_stats(slfw *s, slfw_stats *out);

/* Replace the stream's sample rate -- the one GLOBAL_STATUS gave at open --
 * with one measured from the stream itself, recompute the nominal clock and
 * restart the fit (a new seed). For a console whose status register is wrong
 * while it boots. Run-loop thread only; a no-op for 0 or the current rate. */
void slfw_override_rate(slfw *s, unsigned rate);

#endif
