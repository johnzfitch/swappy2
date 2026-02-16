#include <glib.h>
#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>

#include "algebra.h"
#include "swappy.h"
#include "util.h"

#define pango_layout_t PangoLayout
#define pango_font_description_t PangoFontDescription
#define pango_rectangle_t PangoRectangle

/*
 * Pixelate surface - non-reversible privacy redaction
 * Divides the region into blocks and fills each with the average color
 */
static cairo_surface_t *blur_surface(cairo_surface_t *surface, double x,
                                     double y, double width, double height) {
  cairo_surface_t *dest_surface, *final = NULL;
  cairo_t *cr;
  int src_width, src_height;
  int src_stride, dst_stride;
  uint8_t *src_data, *dst_data;
  uint32_t *s, *d, p;
  int i, j, bi, bj;
  const int block_size = 12;  // Size of pixelation blocks
  gdouble scale_x, scale_y;

  if (cairo_surface_status(surface)) {
    return NULL;
  }

  cairo_surface_get_device_scale(surface, &scale_x, &scale_y);

  cairo_format_t src_format = cairo_image_surface_get_format(surface);
  switch (src_format) {
    case CAIRO_FORMAT_A1:
    case CAIRO_FORMAT_A8:
    default:
      g_warning("source surface format: %d is not supported", src_format);
      return NULL;
    case CAIRO_FORMAT_RGB24:
    case CAIRO_FORMAT_ARGB32:
      break;
  }

  src_stride = cairo_image_surface_get_stride(surface);
  src_width = cairo_image_surface_get_width(surface);
  src_height = cairo_image_surface_get_height(surface);

  dest_surface = cairo_image_surface_create(src_format, src_width, src_height);
  cairo_surface_set_device_scale(dest_surface, scale_x, scale_y);

  if (cairo_surface_status(dest_surface)) {
    return NULL;
  }

  // Copy original surface to dest
  cr = cairo_create(dest_surface);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  cairo_destroy(cr);

  src_data = cairo_image_surface_get_data(surface);
  dst_data = cairo_image_surface_get_data(dest_surface);
  dst_stride = cairo_image_surface_get_stride(dest_surface);

  int start_x = CLAMP((int)(x * scale_x), 0, src_width);
  int start_y = CLAMP((int)(y * scale_y), 0, src_height);
  int end_x = CLAMP((int)((x + width) * scale_x), 0, src_width);
  int end_y = CLAMP((int)((y + height) * scale_y), 0, src_height);

  int scaled_block = (int)(block_size * scale_x);
  if (scaled_block < 4) scaled_block = 4;

  // Process each block
  for (i = start_y; i < end_y; i += scaled_block) {
    for (j = start_x; j < end_x; j += scaled_block) {
      guint64 sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
      int count = 0;
      int block_end_y = MIN(i + scaled_block, end_y);
      int block_end_x = MIN(j + scaled_block, end_x);

      // Calculate average color for this block
      for (bi = i; bi < block_end_y; bi++) {
        s = (uint32_t *)(src_data + bi * src_stride);
        for (bj = j; bj < block_end_x; bj++) {
          p = s[bj];
          sum_a += (p >> 24) & 0xff;
          sum_r += (p >> 16) & 0xff;
          sum_g += (p >> 8) & 0xff;
          sum_b += p & 0xff;
          count++;
        }
      }

      if (count > 0) {
        uint32_t avg_color = ((sum_a / count) << 24) |
                             ((sum_r / count) << 16) |
                             ((sum_g / count) << 8) |
                             (sum_b / count);

        // Fill block with average color
        for (bi = i; bi < block_end_y; bi++) {
          d = (uint32_t *)(dst_data + bi * dst_stride);
          for (bj = j; bj < block_end_x; bj++) {
            d[bj] = avg_color;
          }
        }
      }
    }
  }

  cairo_surface_mark_dirty(dest_surface);

  final = cairo_image_surface_create(src_format, (int)(width * scale_x),
                                     (int)(height * scale_y));

  if (cairo_surface_status(final)) {
    cairo_surface_destroy(dest_surface);
    return NULL;
  }

  cairo_surface_set_device_scale(final, scale_x, scale_y);
  cr = cairo_create(final);
  cairo_set_source_surface(cr, dest_surface, -x, -y);
  cairo_paint(cr);
  cairo_destroy(cr);

  cairo_surface_destroy(dest_surface);
  return final;
}

static void convert_pango_rectangle_to_swappy_box(pango_rectangle_t rectangle,
                                                  struct swappy_box *box) {
  if (!box) {
    return;
  }

  box->x = pango_units_to_double(rectangle.x);
  box->y = pango_units_to_double(rectangle.y);
  box->width = pango_units_to_double(rectangle.width);
  box->height = pango_units_to_double(rectangle.height);
}

static void render_text(cairo_t *cr, struct swappy_paint_text text,
                        struct swappy_state *state) {
  char pango_font[255];
  double x = fmin(text.from.x, text.to.x);
  double y = fmin(text.from.y, text.to.y);
  double w = fabs(text.from.x - text.to.x);
  double h = fabs(text.from.y - text.to.y);

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *crt = cairo_create(surface);

  pango_layout_t *layout = pango_cairo_create_layout(crt);
  pango_layout_set_text(layout, text.text, -1);
  g_snprintf(pango_font, 255, "%s %d", text.font, (int)text.s);
  pango_font_description_t *desc =
      pango_font_description_from_string(pango_font);
  pango_layout_set_width(layout, pango_units_from_double(w));
  pango_layout_set_font_description(layout, desc);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_font_description_free(desc);

  if (text.mode == SWAPPY_TEXT_MODE_EDIT) {
    pango_rectangle_t strong_pos;
    struct swappy_box cursor_box;
    cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 0.3);
    cairo_set_line_width(cr, 5);
    cairo_rectangle(cr, x, y, w, h);
    cairo_stroke(cr);
    glong bytes_til_cursor = string_get_nb_bytes_until(text.text, text.cursor);
    pango_layout_get_cursor_pos(layout, bytes_til_cursor, &strong_pos, NULL);
    convert_pango_rectangle_to_swappy_box(strong_pos, &cursor_box);
    cairo_move_to(crt, cursor_box.x, cursor_box.y);
    cairo_set_source_rgba(crt, 0.3, 0.3, 0.3, 1);
    cairo_line_to(crt, cursor_box.x, cursor_box.y + cursor_box.height);
    cairo_stroke(crt);
    GdkRectangle area = {x + cursor_box.x, y + cursor_box.y + cursor_box.height,
                         0, 0};
    gtk_im_context_set_cursor_location(state->ui->im_context, &area);
  }

  cairo_rectangle(crt, 0, 0, w, h);
  cairo_set_source_rgba(crt, text.r, text.g, text.b, text.a);
  cairo_move_to(crt, 0, 0);
  pango_cairo_show_layout(crt, layout);

  cairo_set_source_surface(cr, surface, x, y);
  cairo_paint(cr);

  cairo_destroy(crt);
  cairo_surface_destroy(surface);
  g_object_unref(layout);
}

static void render_shape_arrow(cairo_t *cr, struct swappy_paint_shape shape) {
  cairo_set_source_rgba(cr, shape.r, shape.g, shape.b, shape.a);
  cairo_set_line_width(cr, shape.w);

  double ftx = shape.to.x - shape.from.x;
  double fty = shape.to.y - shape.from.y;
  double ftn = sqrt(ftx * ftx + fty * fty);

  double r = 20;
  double scaling_factor = shape.w / 4;

  double alpha = G_PI / 6;
  double ta = 5 * alpha;
  double tb = 7 * alpha;
  double xa = r * cos(ta);
  double ya = r * sin(ta);
  double xb = r * cos(tb);
  double yb = r * sin(tb);
  double xc = ftn - fabs(xa) * scaling_factor;

  if (xc < DBL_EPSILON) {
    xc = 0;
  }

  if (ftn < DBL_EPSILON) {
    return;
  }

  double theta = copysign(1.0, fty) * acos(ftx / ftn);

  // Draw line
  cairo_save(cr);
  cairo_translate(cr, shape.from.x, shape.from.y);
  cairo_rotate(cr, theta);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, xc, 0);
  cairo_stroke(cr);
  cairo_restore(cr);

  // Draw arrow
  cairo_save(cr);
  cairo_translate(cr, shape.to.x, shape.to.y);
  cairo_rotate(cr, theta);
  cairo_scale(cr, scaling_factor, scaling_factor);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, xa, ya);
  cairo_line_to(cr, xb, yb);
  cairo_line_to(cr, 0, 0);
  cairo_fill(cr);
  cairo_restore(cr);
}

static void render_shape_line(cairo_t *cr, struct swappy_paint_shape shape) {
  cairo_set_source_rgba(cr, shape.r, shape.g, shape.b, shape.a);
  cairo_set_line_width(cr, shape.w);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr, shape.from.x, shape.from.y);
  cairo_line_to(cr, shape.to.x, shape.to.y);
  cairo_stroke(cr);
}

static void render_highlighter(cairo_t *cr, struct swappy_paint_brush brush) {
  GList *points = brush.points;
  if (!points) {
    return;
  }

  // Highlighter: wide, semi-transparent, flat caps
  cairo_set_source_rgba(cr, brush.r, brush.g, brush.b, 0.4);
  cairo_set_line_width(cr, brush.w * 3);  // Wider than normal brush
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  struct swappy_point *point = points->data;
  cairo_move_to(cr, point->x, point->y);

  for (GList *elem = points->next; elem; elem = elem->next) {
    point = elem->data;
    cairo_line_to(cr, point->x, point->y);
  }
  cairo_stroke(cr);
}

static void render_shape_ellipse(cairo_t *cr, struct swappy_paint_shape shape) {
  double x = fabs(shape.from.x - shape.to.x);
  double y = fabs(shape.from.y - shape.to.y);

  double n = sqrt(x * x + y * y);

  double xc, yc, r;

  if (shape.should_center_at_from) {
    xc = shape.from.x;
    yc = shape.from.y;

    r = n;
  } else {
    xc = shape.from.x + ((shape.to.x - shape.from.x) / 2);
    yc = shape.from.y + ((shape.to.y - shape.from.y) / 2);

    r = n / 2;
  }

  cairo_set_source_rgba(cr, shape.r, shape.g, shape.b, shape.a);
  cairo_set_line_width(cr, shape.w);

  cairo_matrix_t save_matrix;
  cairo_get_matrix(cr, &save_matrix);
  cairo_translate(cr, xc, yc);
  cairo_scale(cr, x / n, y / n);
  cairo_arc(cr, 0, 0, r, 0, 2 * G_PI);
  cairo_set_matrix(cr, &save_matrix);

  switch (shape.operation) {
    case SWAPPY_PAINT_SHAPE_OPERATION_STROKE:
      cairo_stroke(cr);
      break;
    case SWAPPY_PAINT_SHAPE_OPERATION_FILL:
      cairo_fill(cr);
      break;
    default:
      cairo_stroke(cr);
      break;
  }

  cairo_close_path(cr);
}

static void render_shape_rectangle(cairo_t *cr,
                                   struct swappy_paint_shape shape) {
  double x, y, w, h;

  if (shape.should_center_at_from) {
    x = shape.from.x - fabs(shape.from.x - shape.to.x);
    y = shape.from.y - fabs(shape.from.y - shape.to.y);
    w = fabs(shape.from.x - shape.to.x) * 2;
    h = fabs(shape.from.y - shape.to.y) * 2;
  } else {
    x = fmin(shape.from.x, shape.to.x);
    y = fmin(shape.from.y, shape.to.y);
    w = fabs(shape.from.x - shape.to.x);
    h = fabs(shape.from.y - shape.to.y);
  }

  cairo_set_source_rgba(cr, shape.r, shape.g, shape.b, shape.a);
  cairo_set_line_width(cr, shape.w);

  cairo_rectangle(cr, x, y, w, h);
  cairo_close_path(cr);

  switch (shape.operation) {
    case SWAPPY_PAINT_SHAPE_OPERATION_STROKE:
      cairo_stroke(cr);
      break;
    case SWAPPY_PAINT_SHAPE_OPERATION_FILL:
      cairo_fill(cr);
      break;
    default:
      cairo_stroke(cr);
      break;
  }
}

static void render_shape(cairo_t *cr, struct swappy_paint_shape shape) {
  cairo_save(cr);
  switch (shape.type) {
    case SWAPPY_PAINT_MODE_RECTANGLE:
      render_shape_rectangle(cr, shape);
      break;
    case SWAPPY_PAINT_MODE_ELLIPSE:
      render_shape_ellipse(cr, shape);
      break;
    case SWAPPY_PAINT_MODE_ARROW:
      render_shape_arrow(cr, shape);
      break;
    case SWAPPY_PAINT_MODE_LINE:
      render_shape_line(cr, shape);
      break;
    default:
      break;
  }
  cairo_restore(cr);
}

static void render_crop_overlay(cairo_t *cr, struct swappy_paint_shape shape,
                                int image_width, int image_height) {
  double x, y, w, h;

  // Calculate crop rectangle
  if (shape.should_center_at_from) {
    x = shape.from.x - fabs(shape.from.x - shape.to.x);
    y = shape.from.y - fabs(shape.from.y - shape.to.y);
    w = fabs(shape.from.x - shape.to.x) * 2;
    h = fabs(shape.from.y - shape.to.y) * 2;
  } else {
    x = fmin(shape.from.x, shape.to.x);
    y = fmin(shape.from.y, shape.to.y);
    w = fabs(shape.from.x - shape.to.x);
    h = fabs(shape.from.y - shape.to.y);
  }

  cairo_save(cr);

  // Draw dark overlay ONLY outside the crop region using even-odd fill rule
  // Outer rectangle (full image) + inner rectangle (crop area) with even-odd = fills only outside
  cairo_set_source_rgba(cr, 0, 0, 0, 0.5);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle(cr, 0, 0, image_width, image_height);  // Outer
  cairo_rectangle(cr, x, y, w, h);                        // Inner (hole)
  cairo_fill(cr);

  // Draw border around crop region (white solid)
  cairo_set_source_rgba(cr, 1, 1, 1, 1);
  cairo_set_line_width(cr, 2);
  cairo_rectangle(cr, x, y, w, h);
  cairo_stroke(cr);

  // Draw dashed inner border (black)
  double dashes[] = {5.0, 5.0};
  cairo_set_dash(cr, dashes, 2, 0);
  cairo_set_source_rgba(cr, 0, 0, 0, 1);
  cairo_rectangle(cr, x, y, w, h);
  cairo_stroke(cr);

  cairo_restore(cr);
}

static void clear_surface(cairo_t *cr) {
  cairo_save(cr);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_restore(cr);
}

static void render_blur(cairo_t *cr, struct swappy_paint *paint) {
  struct swappy_paint_blur blur = paint->content.blur;

  cairo_surface_t *target = cairo_get_target(cr);

  double x = MIN(blur.from.x, blur.to.x);
  double y = MIN(blur.from.y, blur.to.y);
  double w = ABS(blur.from.x - blur.to.x);
  double h = ABS(blur.from.y - blur.to.y);

  cairo_save(cr);

  if (paint->is_committed) {
    // Surface has already been blurred, reuse it in future passes
    if (blur.surface) {
      cairo_surface_t *surface = blur.surface;
      if (surface && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
        cairo_set_source_surface(cr, surface, x, y);
        cairo_paint(cr);
      }
    } else {
      // Blur surface and reuse it in future passes
      g_info(
          "blurring surface on following image coordinates: %.2lf,%.2lf size: "
          "%.2lfx%.2lf",
          x, y, w, h);
      cairo_surface_t *blurred = blur_surface(target, x, y, w, h);

      if (blurred && cairo_surface_status(blurred) == CAIRO_STATUS_SUCCESS) {
        cairo_set_source_surface(cr, blurred, x, y);
        cairo_paint(cr);
        paint->content.blur.surface = blurred;
      }
    }
  } else {
    // Blur not committed yet, draw bounding rectangle
    struct swappy_paint_shape rect = {
        .r = 0,
        .g = 0.5,
        .b = 1,
        .a = 0.5,
        .w = 5,
        .from = blur.from,
        .to = blur.to,
        .type = SWAPPY_PAINT_MODE_RECTANGLE,
        .operation = SWAPPY_PAINT_SHAPE_OPERATION_FILL,
    };
    render_shape_rectangle(cr, rect);
  }

  cairo_restore(cr);
}

static void render_brush(cairo_t *cr, struct swappy_paint_brush brush) {
  cairo_set_source_rgba(cr, brush.r, brush.g, brush.b, brush.a);
  cairo_set_line_width(cr, brush.w);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_BEVEL);

  guint l = g_list_length(brush.points);

  if (l == 1) {
    struct swappy_point *point = g_list_nth_data(brush.points, 0);
    cairo_rectangle(cr, point->x, point->y, brush.w, brush.w);
    cairo_fill(cr);
  } else {
    for (GList *elem = brush.points; elem; elem = elem->next) {
      struct swappy_point *point = elem->data;
      cairo_line_to(cr, point->x, point->y);
    }
    cairo_stroke(cr);
  }
}

static void render_image(cairo_t *cr, struct swappy_state *state) {
  cairo_surface_t *surface = state->original_image_surface;

  cairo_save(cr);

  if (surface && !cairo_surface_status(surface)) {
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
  }

  cairo_restore(cr);
}

static void render_paint(cairo_t *cr, struct swappy_paint *paint,
                         struct swappy_state *state) {
  if (!paint->can_draw) {
    return;
  }
  switch (paint->type) {
    case SWAPPY_PAINT_MODE_BLUR:
      render_blur(cr, paint);
      break;
    case SWAPPY_PAINT_MODE_BRUSH:
      render_brush(cr, paint->content.brush);
      break;
    case SWAPPY_PAINT_MODE_HIGHLIGHTER:
      render_highlighter(cr, paint->content.brush);
      break;
    case SWAPPY_PAINT_MODE_RECTANGLE:
    case SWAPPY_PAINT_MODE_ELLIPSE:
    case SWAPPY_PAINT_MODE_ARROW:
    case SWAPPY_PAINT_MODE_LINE:
      render_shape(cr, paint->content.shape);
      break;
    case SWAPPY_PAINT_MODE_TEXT:
      render_text(cr, paint->content.text, state);
      break;
    case SWAPPY_PAINT_MODE_CROP: {
      int image_width = gdk_pixbuf_get_width(state->original_image);
      int image_height = gdk_pixbuf_get_height(state->original_image);
      render_crop_overlay(cr, paint->content.shape, image_width, image_height);
      break;
    }
    default:
      g_info("unable to render paint with type: %d", paint->type);
      break;
  }
}

static void render_paints(cairo_t *cr, struct swappy_state *state) {
  for (GList *elem = g_list_last(state->paints); elem; elem = elem->prev) {
    struct swappy_paint *paint = elem->data;
    render_paint(cr, paint, state);
  }

  if (state->temp_paint) {
    render_paint(cr, state->temp_paint, state);
  }
}

void render_state(struct swappy_state *state) {
  cairo_surface_t *surface = state->rendering_surface;
  cairo_t *cr = cairo_create(surface);

  clear_surface(cr);
  render_image(cr, state);
  render_paints(cr, state);

  cairo_destroy(cr);

  /* Invalidate enhanced preview cache since content changed */
  if (state->enhanced_surface) {
    cairo_surface_destroy(state->enhanced_surface);
    state->enhanced_surface = NULL;
  }
  state->enhanced_preset_cache = -1;
  if (state->upscaled_preview_surface) {
    cairo_surface_destroy(state->upscaled_preview_surface);
    state->upscaled_preview_surface = NULL;
  }
  if (state->upscaled_pixbuf_cache) {
    g_object_unref(state->upscaled_pixbuf_cache);
    state->upscaled_pixbuf_cache = NULL;
  }
  state->upscaled_preview_scale_x = 1.0;
  state->upscaled_preview_scale_y = 1.0;
  state->upscaled_preview_cache_valid = FALSE;

  // Drawing is finished, notify the GtkDrawingArea it needs to be redrawn.
  if (state->ui && state->ui->area && GTK_IS_WIDGET(state->ui->area)) {
    gtk_widget_queue_draw(state->ui->area);
  }
}
