#pragma once

/*
 * One layout, two panel sizes. Every drawn screen -- the panel, the clock, the
 * ring screen, Echo, the spectrum, the overlays -- was designed in 1:1 pixels
 * on the S3's 466 px round panel, and the numbers in the code are those pixels.
 * PX() and PXF() turn a design pixel into a pixel on this board's panel, so the
 * P4's 800 px panel shows the same picture at 800/466 (about 1.72x).
 *
 * EXACT AT 466. PX(v) is integer arithmetic that rounds half away from zero,
 * so at PANEL_SIZE 466 it returns v itself; PXF(v) multiplies by 466/466.0f,
 * which is exactly 1. The S3 build therefore compiles to the code it had
 * before these macros existed.
 *
 * WHAT DOES NOT SCALE. Touch thresholds (how far a finger travels before a tap
 * becomes a swipe, how wide the edge bands are, hit slop) stay in panel pixels:
 * a finger is the same size on both boards, and the two panels' pixel density
 * is within 15 % (S3 ~266 ppi, P4 ~235 ppi). One-pixel antialiasing ramps and
 * the tile size of the mask builder stay too: they are about pixels, not about
 * the design.
 *
 * Plain C with no includes, so host/panel.sh and host/clock.sh compile it as
 * is. PANEL_SIZE comes from the build: main/CMakeLists.txt for the firmware,
 * -DPANEL_SIZE=... for the host harnesses.
 */

#ifndef PANEL_SIZE
#error "PANEL_SIZE comes from main/CMakeLists.txt, or -DPANEL_SIZE on the host"
#endif

#define UI_DESIGN_SIZE 466

/* A design pixel count, as an int. Integer constant expression: fine in
 * #defines, static initialisers and array sizes. Integers only. */
#define PX(v) (((v) * PANEL_SIZE + ((v) < 0 ? -(UI_DESIGN_SIZE / 2) : UI_DESIGN_SIZE / 2)) / UI_DESIGN_SIZE)

/* A design pixel distance, as a float. */
#define PXF(v) ((v) * ((float)PANEL_SIZE / (float)UI_DESIGN_SIZE))
