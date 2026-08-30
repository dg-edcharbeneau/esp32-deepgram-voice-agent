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

/*
 * Registers the allocation-failure hook and starts the sampler.
 *
 * Call EARLY in app_main, before audio_io_init(), so the hook is armed before
 * anything large is allocated.
 */
void heap_probe_start(void);
