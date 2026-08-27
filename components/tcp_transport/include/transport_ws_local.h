/*
 * The local patches' public surface -- all of it.
 *
 * components/tcp_transport is an override of ESP-IDF's component by name,
 * carrying one modified file (transport_ws.c) and taking everything else out of
 * $IDF_PATH. This header exists so the one thing those patches produce that is
 * worth reading from outside -- how much audio the transport has thrown away --
 * does not have to be reached by declaring an extern somewhere else and hoping
 * the two stay in step.
 *
 * DELIBERATELY NOT NAMED esp_transport_ws_*. Everything with that prefix is
 * upstream API that will still be there when these patches are deleted; nothing
 * here will be. Keeping the prefixes apart is what makes a stale call site
 * obvious at the point of use rather than at the point of removal.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Self-contained binary frames LOCAL PATCH 2 has dropped, cumulative since boot,
 * across every WebSocket transport in the image.
 *
 * A drop means the socket was not writable inside the deadline and the frame was
 * discarded rather than the session being torn down -- so this counts audio the
 * far end never heard. It is the LOWER of the uplink's two loss paths; the upper
 * one is dg_agent's queue, which drops before a frame is ever offered here. Both
 * ride the TLM line, as txdrop= and updrop= respectively, and they mean
 * different things: updrop says the queue was still full, txdrop says the socket
 * refused. A device shedding audio with updrop=0 is being throttled here.
 *
 * Safe from any task: one relaxed atomic load.
 */
uint32_t transport_ws_local_dropped_frames(void);

#ifdef __cplusplus
}
#endif
