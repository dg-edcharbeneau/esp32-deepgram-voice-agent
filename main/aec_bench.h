/*
 * Prices esp-sr's standalone AEC against Espressif's own test vectors.
 *
 * WHY A BENCH RATHER THAN THE ROOM
 *
 * Every measurement this project has made so far -- the linearity sweep, the ERL
 * table, the -11 dB signal-to-echo estimate -- answers "can this room be
 * cancelled" using an integration nobody has validated, driven by a stimulus (the
 * agent's greeting) that differs every run. If the wiring is wrong, none of those
 * numbers mean anything, and there is no way to tell from the numbers themselves.
 *
 * Espressif ships a far/near pair and the output their own AEC produces from it.
 * Running the same pair through our integration and comparing ERLE against theirs
 * separates the two questions: ours matching theirs says the wiring is right,
 * independent of this room, this speaker and this microphone.
 *
 * The vectors live in the `storage` SPIFFS partition, which was already in
 * partitions.csv and unused. See the root CMakeLists.txt.
 *
 * DEFAULT OFF. This links esp-sr, allocates a canceller and reads 6 MB of flash;
 * none of it belongs in a shipping build.
 */
#pragma once

/*
 * Runs the whole sweep and logs it. Call ONCE, from app_main, AFTER ui_start() --
 * the display's ~29.8 kB contiguous allocation has to have happened already or
 * every heap figure here describes a state the device is never in.
 *
 * Blocking, and slow: several modes x ~53 s of audio each, read from flash.
 */
void aec_bench_run(void);
