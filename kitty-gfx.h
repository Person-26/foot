#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pixman.h>

/*
 * A minimal subset of the kitty graphics protocol - enough for a program to
 * upload images once and then move them around smoothly (e.g. a PDF viewer
 * scrolling by pixels). Supported:
 *
 *   a=t / a=T  transmit (and, with T, place): t=s (POSIX shared memory,
 *              unlinked after reading) or t=d (direct, uncompressed);
 *              f=24 (RGB) or f=32 (RGBA); s, v, i
 *   a=p        place: i, p, x, y, w, h (source rectangle), X, Y (pixel offset
 *              within the cursor's cell), z (only >= 0: drawn above text).
 *              The cursor never moves (as if C=1).
 *   a=d        delete: d=a/A (all), d=i/I (by i, optionally p); upper case
 *              also frees the image data
 *   a=q        query
 *   q=0/1/2    reply verbosity
 *
 * Placements are anchored to screen cells, not to scrollback, and belong to
 * the screen (normal/alternate) they were made on. Leaving the alternate
 * screen frees everything made on it.
 */

struct terminal;

void kitty_gfx_apc(struct terminal *term, const char *data, size_t len);
/* Before a frame's rows are rendered: re-renders the cells a placement has
 * left, and skips the ones hidden under an opaque image */
void kitty_gfx_prepare(struct terminal *term, bool scrolled);
void kitty_gfx_render(struct terminal *term, pixman_image_t *pix,
                      pixman_region32_t *damage);
void kitty_gfx_reset(struct terminal *term);
void kitty_gfx_leave_alt(struct terminal *term);
