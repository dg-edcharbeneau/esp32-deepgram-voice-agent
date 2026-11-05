/*
 * Finds out what consumes internal RAM, rather than guessing.
 *
 * WHY
 *
 * With the canceller running and the mic gate off, the device drops its Deepgram
 * session every ~15 s: esp-aes cannot allocate, the TLS write fails, the socket
 * closes, the client reconnects. Free internal RAM sits at 44-47 kB and then
 * collapses to 11 kB inside two seconds, and NOTHING is logged in between -- the
 * TLM line samples once a second, which is too coarse to say whether that is one
 * allocation or a hundred.
 *
 * Three fixes were proposed off the back of that (dynamic TLS buffers, moving the
 * capture buffer to PSRAM, trimming DMA depth). All three are guesses. This names
 * the consumer instead.
 *
 * DEFAULT OFF. It logs from an allocation-failure hook, which runs in whatever
 * context failed -- including an ISR -- so it is a debug tool, not a resident.
 */
#pragma once

#include <stdint.h>

/*
 * Registers the allocation-failure hook and starts the sampler.
 *
 * Call EARLY in app_main, before audio_io_init(), so the hook is armed before
 * anything large is allocated.
 */
void heap_probe_start(void);

/*
 * The lowest internal-RAM figures seen since boot, sampled every 50 ms.
 *
 * This exists because the sampler was already tracking them and nothing ever
 * read them back -- every long run this firmware has done recorded its own worst
 * case and then threw it away. The TLM line samples once a second and cannot see
 * a collapse between samples; these can.
 *
 * `dma_largest` is the figure that decides whether esp-aes can bounce a TLS
 * record, and is therefore the one to quote. MALLOC_CAP_DMA is a strict SUBSET
 * of MALLOC_CAP_INTERNAL, so `largest` over-reports what that allocation can
 * actually get.
 *
 * Any pointer may be NULL. With CONFIG_HEAP_PROBE off there is no sampler and
 * every out-param is set to UINT32_MAX, which reads as "never sampled" rather
 * than as a floor of zero -- a caller that prints it unguarded will show an
 * absurd number instead of a plausible lie.
 *
 * A LOW-WATER MARK IS A SAMPLE, NOT A BOUND. It is the worst that was seen, not
 * the worst that exists; a path that never ran cannot appear in it.
 */
void heap_probe_get_minima(uint32_t *free, uint32_t *largest,
                           uint32_t *dma_largest);
