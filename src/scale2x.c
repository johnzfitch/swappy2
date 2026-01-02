/*
 * scale2x.c - EPX/Scale2x pixel art upscaling for screenshots
 * 
 * This is the simplest "smart" upscaling algorithm. It was designed for
 * pixel art but works great on screenshots because they share similar
 * properties: hard edges, limited colors, axis-aligned features.
 * 
 * Compile: gcc -O3 -march=native -o scale2x scale2x.c
 * 
 * For integration with Cairo/GdkPixbuf, see scale2x_surface() below.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>

#include "scale2x.h"

/* Scale2x (EPX) - Original algorithm by Andrea Mazzoleni
 * 
 * For each pixel P with neighbors:
 *     A
 *   C P B
 *     D
 * 
 * Create 2x2 output:
 *   1 2
 *   3 4
 * 
 * Rules:
 *   1 = (C == A && C != D && A != B) ? A : P
 *   2 = (A == B && A != C && B != D) ? B : P
 *   3 = (D == C && D != B && C != A) ? C : P
 *   4 = (B == D && B != A && D != C) ? D : P
 */

static inline int pixels_equal(uint32_t a, uint32_t b) {
    /* For screenshots, exact match is usually fine.
     * For anti-aliased edges, you might want a threshold. */
    return a == b;
}

static inline int pixels_equal_threshold(uint32_t a, uint32_t b, int threshold) {
    /* Color distance for handling anti-aliased edges */
    int ra = (a >> 16) & 0xff;
    int ga = (a >> 8) & 0xff;
    int ba = a & 0xff;
    int rb = (b >> 16) & 0xff;
    int gb = (b >> 8) & 0xff;
    int bb = b & 0xff;
    
    int dr = ra - rb;
    int dg = ga - gb;
    int db = ba - bb;
    
    /* Weighted luma difference */
    int diff = abs(dr) * 299 + abs(dg) * 587 + abs(db) * 114;
    return diff < threshold * 1000;
}

/* Basic Scale2x - 2x upscale */
void scale2x(const uint32_t *src, uint32_t *dst, int w, int h) {
    int dw = w * 2;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Sample center and neighbors (clamped at edges) */
            uint32_t P = src[y * w + x];
            uint32_t A = (y > 0)     ? src[(y-1) * w + x] : P;
            uint32_t B = (x < w-1)   ? src[y * w + x + 1] : P;
            uint32_t C = (x > 0)     ? src[y * w + x - 1] : P;
            uint32_t D = (y < h-1)   ? src[(y+1) * w + x] : P;
            
            int dx = x * 2;
            int dy = y * 2;
            
            /* Apply EPX rules */
            int ca = pixels_equal(C, A);
            int ab = pixels_equal(A, B);
            int bd = pixels_equal(B, D);
            int dc = pixels_equal(D, C);
            int cd = pixels_equal(C, D);
            int ac = pixels_equal(A, C);
            int ba = pixels_equal(B, A);
            int db = pixels_equal(D, B);
            
            dst[dy * dw + dx]           = (ca && !cd && !ab) ? A : P;
            dst[dy * dw + dx + 1]       = (ab && !ac && !bd) ? B : P;
            dst[(dy+1) * dw + dx]       = (dc && !db && !ac) ? C : P;
            dst[(dy+1) * dw + dx + 1]   = (bd && !ba && !dc) ? D : P;
        }
    }
}

/* Scale3x - 3x upscale (more complex rules) */
void scale3x(const uint32_t *src, uint32_t *dst, int w, int h) {
    int dw = w * 3;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            /* Sample 3x3 neighborhood */
            uint32_t A = (y > 0 && x > 0)       ? src[(y-1)*w + x-1] : src[y*w + x];
            uint32_t B = (y > 0)                ? src[(y-1)*w + x]   : src[y*w + x];
            uint32_t C = (y > 0 && x < w-1)     ? src[(y-1)*w + x+1] : src[y*w + x];
            uint32_t D = (x > 0)                ? src[y*w + x-1]     : src[y*w + x];
            uint32_t E = src[y*w + x];  /* Center */
            uint32_t F = (x < w-1)              ? src[y*w + x+1]     : src[y*w + x];
            uint32_t G = (y < h-1 && x > 0)     ? src[(y+1)*w + x-1] : src[y*w + x];
            uint32_t H = (y < h-1)              ? src[(y+1)*w + x]   : src[y*w + x];
            uint32_t I = (y < h-1 && x < w-1)   ? src[(y+1)*w + x+1] : src[y*w + x];
            
            int dx = x * 3;
            int dy = y * 3;
            
            #define EQ(a, b) pixels_equal(a, b)
            #define NE(a, b) (!pixels_equal(a, b))
            
            /* Scale3x rules */
            dst[dy*dw + dx]         = (EQ(D,B) && NE(D,H) && NE(B,F)) ? D : E;
            dst[dy*dw + dx+1]       = ((EQ(D,B) && NE(D,H) && NE(B,F) && NE(E,C)) || 
                                       (EQ(B,F) && NE(B,D) && NE(F,H) && NE(E,A))) ? B : E;
            dst[dy*dw + dx+2]       = (EQ(B,F) && NE(B,D) && NE(F,H)) ? F : E;
            
            dst[(dy+1)*dw + dx]     = ((EQ(D,B) && NE(D,H) && NE(B,F) && NE(E,G)) || 
                                       (EQ(D,H) && NE(D,B) && NE(H,F) && NE(E,A))) ? D : E;
            dst[(dy+1)*dw + dx+1]   = E;
            dst[(dy+1)*dw + dx+2]   = ((EQ(B,F) && NE(B,D) && NE(F,H) && NE(E,I)) || 
                                       (EQ(H,F) && NE(D,H) && NE(B,F) && NE(E,C))) ? F : E;
            
            dst[(dy+2)*dw + dx]     = (EQ(D,H) && NE(D,B) && NE(H,F)) ? D : E;
            dst[(dy+2)*dw + dx+1]   = ((EQ(D,H) && NE(D,B) && NE(H,F) && NE(E,I)) || 
                                       (EQ(H,F) && NE(D,H) && NE(B,F) && NE(E,G))) ? H : E;
            dst[(dy+2)*dw + dx+2]   = (EQ(H,F) && NE(D,H) && NE(B,F)) ? F : E;
            
            #undef EQ
            #undef NE
        }
    }
}

/* Multi-pass for higher scales: 2->4->8 etc */
uint32_t* scale_nx(const uint32_t *src, int w, int h, int scale, int *out_w, int *out_h) {
    if (scale < 2) {
        *out_w = w;
        *out_h = h;
        uint32_t *copy = malloc(w * h * sizeof(uint32_t));
        memcpy(copy, src, w * h * sizeof(uint32_t));
        return copy;
    }
    
    uint32_t *current = (uint32_t*)src;
    int cur_w = w;
    int cur_h = h;
    uint32_t *next = NULL;
    int owns_current = 0;
    
    /* Apply 2x passes until we reach desired scale */
    while (scale >= 2) {
        int next_w = cur_w * 2;
        int next_h = cur_h * 2;
        next = malloc(next_w * next_h * sizeof(uint32_t));
        
        scale2x(current, next, cur_w, cur_h);
        
        if (owns_current) {
            free(current);
        }
        
        current = next;
        cur_w = next_w;
        cur_h = next_h;
        owns_current = 1;
        scale /= 2;
    }
    
    *out_w = cur_w;
    *out_h = cur_h;
    return current;
}


/* ========================================================================
 * Cairo/GTK Integration
 * ======================================================================== */

/* Convert Cairo surface to pixel array, upscale, convert back */
cairo_surface_t* scale2x_surface(cairo_surface_t *src, int passes) {
    cairo_surface_flush(src);
    
    int w = cairo_image_surface_get_width(src);
    int h = cairo_image_surface_get_height(src);
    int stride = cairo_image_surface_get_stride(src);
    unsigned char *data = cairo_image_surface_get_data(src);
    
    /* Cairo uses ARGB32, same as our uint32_t */
    uint32_t *pixels = malloc(w * h * sizeof(uint32_t));
    
    /* Copy row by row (stride may differ from width) */
    for (int y = 0; y < h; y++) {
        memcpy(&pixels[y * w], &data[y * stride], w * sizeof(uint32_t));
    }
    
    /* Apply scale2x for each pass */
    int scale = 1 << passes;  /* 2^passes */
    int out_w, out_h;
    uint32_t *scaled = scale_nx(pixels, w, h, scale, &out_w, &out_h);
    free(pixels);
    
    /* Create new Cairo surface */
    cairo_surface_t *dst = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, out_w, out_h
    );
    
    unsigned char *dst_data = cairo_image_surface_get_data(dst);
    int dst_stride = cairo_image_surface_get_stride(dst);
    
    for (int y = 0; y < out_h; y++) {
        memcpy(&dst_data[y * dst_stride], &scaled[y * out_w], 
               out_w * sizeof(uint32_t));
    }
    
    cairo_surface_mark_dirty(dst);
    free(scaled);

    return dst;
}

/* Viewport-based upscaling - only upscale the visible region
 * This is the key optimization: higher zoom = smaller source = FASTER
 */
cairo_surface_t* scale2x_viewport(cairo_surface_t *src_surface,
                                   int viewport_x, int viewport_y,
                                   int viewport_w, int viewport_h,
                                   int scale) {
    cairo_surface_flush(src_surface);

    int src_w = cairo_image_surface_get_width(src_surface);
    int src_h = cairo_image_surface_get_height(src_surface);
    int src_stride = cairo_image_surface_get_stride(src_surface);
    unsigned char *src_data = cairo_image_surface_get_data(src_surface);

    /* Clamp viewport to source bounds */
    if (viewport_x < 0) viewport_x = 0;
    if (viewport_y < 0) viewport_y = 0;
    if (viewport_x + viewport_w > src_w) viewport_w = src_w - viewport_x;
    if (viewport_y + viewport_h > src_h) viewport_h = src_h - viewport_y;

    if (viewport_w <= 0 || viewport_h <= 0) {
        return NULL;
    }

    /* Extract viewport region */
    uint32_t *region = malloc(viewport_w * viewport_h * sizeof(uint32_t));
    for (int y = 0; y < viewport_h; y++) {
        int src_y = viewport_y + y;
        uint32_t *src_row = (uint32_t*)(src_data + src_y * src_stride);
        for (int x = 0; x < viewport_w; x++) {
            region[y * viewport_w + x] = src_row[viewport_x + x];
        }
    }

    /* Apply scale2x passes */
    int out_w, out_h;
    uint32_t *scaled = scale_nx(region, viewport_w, viewport_h, scale, &out_w, &out_h);
    free(region);

    /* Create output Cairo surface */
    cairo_surface_t *dst = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, out_w, out_h
    );

    unsigned char *dst_data = cairo_image_surface_get_data(dst);
    int dst_stride = cairo_image_surface_get_stride(dst);

    for (int y = 0; y < out_h; y++) {
        memcpy(dst_data + y * dst_stride, &scaled[y * out_w],
               out_w * sizeof(uint32_t));
    }

    cairo_surface_mark_dirty(dst);
    free(scaled);

    return dst;
}


/* ========================================================================
 * Advanced: Scale2x with threshold (for anti-aliased content)
 * ======================================================================== */

void scale2x_aa(const uint32_t *src, uint32_t *dst, int w, int h, int threshold) {
    int dw = w * 2;
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t P = src[y * w + x];
            uint32_t A = (y > 0)     ? src[(y-1) * w + x] : P;
            uint32_t B = (x < w-1)   ? src[y * w + x + 1] : P;
            uint32_t C = (x > 0)     ? src[y * w + x - 1] : P;
            uint32_t D = (y < h-1)   ? src[(y+1) * w + x] : P;
            
            int dx = x * 2;
            int dy = y * 2;
            
            #define TEQ(a, b) pixels_equal_threshold(a, b, threshold)
            
            int ca = TEQ(C, A);
            int ab = TEQ(A, B);
            int bd = TEQ(B, D);
            int dc = TEQ(D, C);
            int cd = TEQ(C, D);
            int ac = TEQ(A, C);
            int ba = TEQ(B, A);
            int db = TEQ(D, B);
            
            dst[dy * dw + dx]           = (ca && !cd && !ab) ? A : P;
            dst[dy * dw + dx + 1]       = (ab && !ac && !bd) ? B : P;
            dst[(dy+1) * dw + dx]       = (dc && !db && !ac) ? C : P;
            dst[(dy+1) * dw + dx + 1]   = (bd && !ba && !dc) ? D : P;
            
            #undef TEQ
        }
    }
}


/* ========================================================================
 * Test/Demo
 * ======================================================================== */

#ifdef SCALE2X_DEMO
#include <stdio.h>
#include <time.h>

int main() {
    /* Create test pattern: 100x100 with diagonal line */
    int w = 1920, h = 1080;
    uint32_t *src = malloc(w * h * sizeof(uint32_t));
    
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (abs(x - y) < 3) {
                src[y * w + x] = 0xFF00FF00;  /* Green diagonal */
            } else {
                src[y * w + x] = 0xFFFFFFFF;  /* White background */
            }
        }
    }
    
    /* Benchmark */
    int out_w, out_h;
    clock_t start = clock();
    
    for (int i = 0; i < 10; i++) {
        uint32_t *scaled = scale_nx(src, w, h, 2, &out_w, &out_h);
        free(scaled);
    }
    
    clock_t end = clock();
    double ms = ((double)(end - start) / CLOCKS_PER_SEC) * 100;  /* Per iteration */
    
    printf("Scale2x 1920x1080 -> %dx%d: %.2f ms/frame\n", out_w, out_h, ms);
    printf("%.1f FPS\n", 1000.0 / ms);
    
    free(src);
    return 0;
}
#endif
