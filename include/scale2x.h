#pragma once

#include <stdint.h>
#include <cairo.h>

/* Scale2x (EPX) - 2x pixel art upscaling */
void scale2x(const uint32_t *src, uint32_t *dst, int w, int h);

/* Scale3x - 3x upscaling */
void scale3x(const uint32_t *src, uint32_t *dst, int w, int h);

/* Multi-pass scaling to any power-of-2 */
uint32_t* scale_nx(const uint32_t *src, int w, int h, int scale, int *out_w, int *out_h);

/* Upscale a viewport region of a Cairo surface using Scale2x
 * Returns a new surface that must be destroyed by caller
 *
 * src_surface: Source image surface
 * viewport_x, viewport_y: Top-left of visible region in source coords
 * viewport_w, viewport_h: Size of visible region in source coords
 * scale: Scale factor (2, 4, 8, etc)
 */
cairo_surface_t* scale2x_viewport(cairo_surface_t *src_surface,
                                   int viewport_x, int viewport_y,
                                   int viewport_w, int viewport_h,
                                   int scale);
