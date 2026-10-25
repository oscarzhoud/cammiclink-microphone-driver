// Copyright (c) 2026 Oscar Zhou. MIT license; see LICENSE.
// No DSP, FIFO consumption, sample-value writer detection, allocation or locks in IO.
#include "CMLAudioCore.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <mach/mach_time.h>

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "Realtime storage requires lock-free 64-bit atomics");
_Static_assert(sizeof(float) == 4, "PCM is Float32");
_Static_assert((CML_RING_FRAMES & (CML_RING_FRAMES - 1)) == 0, "Power-of-two ring");

// seq_cst is deliberate: invalidation, payload, publication, then the two reader
// tag checks have a single total order even when an old slot is being overwritten.
// A float pair is loaded atomically; a slot never exposes a half-updated channel.
typedef struct { _Atomic uint64_t tag; _Atomic uint64_t pcm; } CMLFrame;
static CMLFrame gFrames[CML_RING_FRAMES];
static _Atomic uint64_t gWriter; // 0 means none, otherwise clientID + 1
static uint64_t gAnchor;
static double gTicksPerFrame;

#ifdef CML_TEST
extern uint64_t CMLTestHostTime(void);
#define CMLNow CMLTestHostTime
#else
#define CMLNow mach_absolute_time
#endif

void CMLInitialize(void) {
    // Called only by plug-in Initialize, before any IO. Never reset on StartIO:
    // two device clocks must remain identical when clients join in any order.
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gTicksPerFrame = 1e9 * (double)tb.denom / (double)tb.numer / CML_SAMPLE_RATE;
    gAnchor = CMLNow();
    atomic_store(&gWriter, 0);
    for (uint32_t i = 0; i < CML_RING_FRAMES; ++i) {
        atomic_store(&gFrames[i].tag, UINT64_MAX);
        atomic_store(&gFrames[i].pcm, 0);
    }
}

int CMLStartIO(uint32_t device, uint32_t client) {
    if (device == CML_INPUT_DEVICE) return 0;
    if (device != CML_WRITER_DEVICE) return -1;
    uint64_t expected = 0, desired = (uint64_t)client + 1;
    if (atomic_compare_exchange_strong(&gWriter, &expected, desired)) return 0;
    return expected == desired ? 0 : -1; // explicit sole writer, including valid zeros
}

void CMLStopIO(uint32_t device, uint32_t client) {
    if (device != CML_WRITER_DEVICE) return;
    uint64_t expected = (uint64_t)client + 1;
    atomic_compare_exchange_strong(&gWriter, &expected, 0);
    // Historical frames drain by timestamp, then are unavailable. No stale looping.
}

void CMLGetZeroTimeStamp(double *sample, uint64_t *host, uint64_t *seed) {
    uint64_t now = CMLNow();
    double elapsed = now >= gAnchor ? (double)(now - gAnchor) / gTicksPerFrame : 0;
    uint64_t periods = (uint64_t)(elapsed / CML_PERIOD_FRAMES);
    *sample = (double)(periods * CML_PERIOD_FRAMES);
    *host = gAnchor + (uint64_t)(*sample * gTicksPerFrame);
    *seed = 1; // shared continuous clock; no discontinuous per-device start resets
}

static bool SampleIndex(double time, uint32_t frames, int64_t *start) {
    if (!isfinite(time) || time < -(double)CML_RING_FRAMES || time > 9e15) return false;
    double rounded = nearbyint(time);
    if (fabs(time - rounded) > .001) return false;
    *start = (int64_t)rounded;
    return frames <= CML_MAX_IO_FRAMES;
}

// Bounded circuit breaker for pathological callback storms. It is intentionally
// well above 64-frame IO with many clients; a fixed 3000/s threshold is too low.
// No logging or uninitialized 'passthrough' output. Future windows can recover.
static _Atomic uint64_t gGuardWindow;
static _Atomic uint32_t gGuardCount;
static bool WithinIOBudget(void) {
    double ticksPerSecond = gTicksPerFrame * CML_SAMPLE_RATE;
    uint64_t window = (uint64_t)((double)CMLNow() / ticksPerSecond);
    uint64_t old = atomic_load_explicit(&gGuardWindow, memory_order_relaxed);
    if (old != window && atomic_compare_exchange_strong(&gGuardWindow, &old, window))
        atomic_store_explicit(&gGuardCount, 0, memory_order_relaxed);
    return atomic_fetch_add_explicit(&gGuardCount, 1, memory_order_relaxed) < 50000;
}

void CMLWrite(const float *src, uint32_t frames, double sampleTime, uint32_t client) {
    int64_t first;
    if (!src || !frames || !SampleIndex(sampleTime, frames, &first) ||
        atomic_load(&gWriter) != (uint64_t)client + 1 || !WithinIOBudget()) return;
    for (uint32_t i = 0; i < frames; ++i) {
        int64_t time = first + i;
        if (time < 0) continue;
        uint64_t bits;
        memcpy(&bits, src + i * CML_CHANNELS, sizeof(bits));
        CMLFrame *slot = &gFrames[(uint64_t)time & (CML_RING_FRAMES - 1)];
        atomic_store(&slot->tag, UINT64_MAX);
        atomic_store(&slot->pcm, bits);
        atomic_store(&slot->tag, (uint64_t)time);
    }
}

void CMLRead(float *dst, uint32_t frames, double sampleTime) {
    if (!dst) return;
    // Caller validates actual buffer capacity from HAL's frame count and ASBD.
    // Do not touch beyond requested frames even on malformed timestamps.
    memset(dst, 0, (size_t)frames * CML_CHANNELS * sizeof(float));
    int64_t first;
    if (!frames || !SampleIndex(sampleTime, frames, &first) || !WithinIOBudget()) return;
    for (uint32_t i = 0; i < frames; ++i) {
        int64_t time = first + i;
        if (time < 0) continue;
        CMLFrame *slot = &gFrames[(uint64_t)time & (CML_RING_FRAMES - 1)];
        uint64_t tag = atomic_load(&slot->tag);
        if (tag != (uint64_t)time) continue;
        uint64_t bits = atomic_load(&slot->pcm);
        if (atomic_load(&slot->tag) != tag) continue;
        memcpy(dst + i * CML_CHANNELS, &bits, sizeof(bits));
    }
}
