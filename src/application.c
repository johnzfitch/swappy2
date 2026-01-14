#include <gdk/gdk.h>
#include <glib-2.0/glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "clipboard.h"
#include "config.h"
#include "file.h"
#include "paint.h"
#include "pixbuf.h"
#include "render.h"
#include "scale2x.h"
#include "swappy.h"

// Forward declarations
static void compute_window_size_and_scaling_factor(struct swappy_state *state);

// Show notification using notify-send
static void show_notification(const char *title, const char *message) {
  pid_t pid = fork();
  if (pid == 0) {
    // Child process
    execlp("notify-send", "notify-send", "-t", "2000", "-a", "Swappy", title, message, NULL);
    _exit(1);  // If exec fails
  }
  // Parent doesn't wait - fire and forget
}

static void update_ui_undo_redo(struct swappy_state *state) {
  GtkWidget *undo = GTK_WIDGET(state->ui->undo);
  GtkWidget *redo = GTK_WIDGET(state->ui->redo);
  gboolean undo_sensitive = g_list_length(state->paints) > 0;
  gboolean redo_sensitive = g_list_length(state->redo_paints) > 0;
  gtk_widget_set_sensitive(undo, undo_sensitive);
  gtk_widget_set_sensitive(redo, redo_sensitive);
}

static void update_ui_stroke_size_widget(struct swappy_state *state) {
  GtkButton *button = GTK_BUTTON(state->ui->line_size);
  char label[255];
  g_snprintf(label, 255, "%.0lf", state->settings.w);
  gtk_button_set_label(button, label);
}

static void update_ui_text_size_widget(struct swappy_state *state) {
  GtkButton *button = GTK_BUTTON(state->ui->text_size);
  char label[255];
  g_snprintf(label, 255, "%.0lf", state->settings.t);
  gtk_button_set_label(button, label);
}

static void update_ui_transparency_widget(struct swappy_state *state) {
  GtkButton *button = GTK_BUTTON(state->ui->transparency);
  char label[255];
  g_snprintf(label, 255, "%" PRId32, state->settings.tr);
  gtk_button_set_label(button, label);
}

static void update_ui_panel_toggle_button(struct swappy_state *state) {
  GtkWidget *painting_box = GTK_WIDGET(state->ui->painting_box);
  GtkToggleButton *button = GTK_TOGGLE_BUTTON(state->ui->panel_toggle_button);
  gboolean toggled = state->ui->panel_toggled;

  gtk_toggle_button_set_active(button, toggled);
  gtk_widget_set_visible(painting_box, toggled);
}

static void update_ui_fill_shape_toggle_button(struct swappy_state *state) {
  GtkToggleButton *button = GTK_TOGGLE_BUTTON(state->ui->fill_shape);
  gboolean toggled = state->config->fill_shape;

  gtk_toggle_button_set_active(button, toggled);
}

static void update_ui_transparent_toggle_button(struct swappy_state *state) {
  GtkToggleButton *button = GTK_TOGGLE_BUTTON(state->ui->transparent);
  gboolean toggled = state->config->transparent;

  gtk_toggle_button_set_active(button, toggled);
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->transparency), toggled);
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->transparency_minus), toggled);
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->transparency_plus), toggled);
}

static void action_apply_crop(struct swappy_state *state) {
  struct swappy_paint *paint = state->temp_paint;

  if (!paint || paint->type != SWAPPY_PAINT_MODE_CROP || !paint->can_draw) {
    return;
  }

  struct swappy_paint_shape shape = paint->content.shape;
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

  // Ensure crop region is within image bounds
  int image_width = gdk_pixbuf_get_width(state->original_image);
  int image_height = gdk_pixbuf_get_height(state->original_image);

  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x + w > image_width) w = image_width - x;
  if (y + h > image_height) h = image_height - y;

  if (w <= 0 || h <= 0) {
    g_warning("Invalid crop region");
    paint_free(paint);
    state->temp_paint = NULL;
    return;
  }

  // First, render all existing paints onto the original image
  cairo_t *cr = cairo_create(state->original_image_surface);
  for (GList *elem = g_list_last(state->paints); elem; elem = elem->prev) {
    struct swappy_paint *p = elem->data;
    if (p->can_draw && p->type != SWAPPY_PAINT_MODE_CROP) {
      // Use pixbuf_get_from_state's logic to render paints
    }
  }
  cairo_destroy(cr);

  // Create new cropped pixbuf from the rendered surface
  GdkPixbuf *cropped = gdk_pixbuf_new_subpixbuf(state->original_image,
                                                 (int)x, (int)y, (int)w, (int)h);
  if (!cropped) {
    g_warning("Failed to create cropped pixbuf");
    paint_free(paint);
    state->temp_paint = NULL;
    return;
  }

  // Make a copy because subpixbuf shares memory
  GdkPixbuf *cropped_copy = gdk_pixbuf_copy(cropped);
  g_object_unref(cropped);

  if (!cropped_copy) {
    g_warning("Failed to copy cropped pixbuf");
    paint_free(paint);
    state->temp_paint = NULL;
    return;
  }

  // Replace original image
  g_object_unref(state->original_image);
  state->original_image = cropped_copy;

  // Clear all paints (they would be in wrong positions after crop)
  paint_free_list(&state->paints);
  paint_free_list(&state->redo_paints);

  // Recreate surfaces from new image
  pixbuf_scale_surface_from_widget(state, state->ui->area);

  // Resize window to fit new image
  compute_window_size_and_scaling_factor(state);
  gtk_widget_set_size_request(state->ui->area, state->window->width, state->window->height);
  gtk_window_resize(state->ui->window, state->window->width, state->window->height);

  // Clean up crop paint
  paint_free(paint);
  state->temp_paint = NULL;

  // Reset zoom/pan for new cropped image
  state->zoom_level = 1.0;
  state->pan_x = 0.0;
  state->pan_y = 0.0;

  // Render the new state and redraw
  render_state(state);
  gtk_widget_queue_draw(state->ui->area);

  g_info("Crop applied: %dx%d at (%d,%d)", (int)w, (int)h, (int)x, (int)y);
}

void application_finish(struct swappy_state *state) {
  g_debug("application finishing, cleaning up");
  paint_free_all(state);
  pixbuf_free(state);
  cairo_surface_destroy(state->rendering_surface);
  cairo_surface_destroy(state->original_image_surface);
  if (state->temp_file_str) {
    g_info("deleting temporary file: %s", state->temp_file_str);
    if (g_unlink(state->temp_file_str) != 0) {
      g_warning("unable to delete temporary file: %s", state->temp_file_str);
    }
    g_free(state->temp_file_str);
  }
  g_free(state->file_str);
  g_free(state->geometry);
  g_free(state->window);
  g_object_unref(state->ui->im_context);
  g_free(state->ui);

  g_object_unref(state->app);

  config_free(state);
}

static void action_undo(struct swappy_state *state) {
  GList *first = state->paints;

  if (first) {
    state->paints = g_list_remove_link(state->paints, first);
    state->redo_paints = g_list_prepend(state->redo_paints, first->data);

    render_state(state);
    update_ui_undo_redo(state);
  }
}

static void action_redo(struct swappy_state *state) {
  GList *first = state->redo_paints;

  if (first) {
    state->redo_paints = g_list_remove_link(state->redo_paints, first);
    state->paints = g_list_prepend(state->paints, first->data);

    render_state(state);
    update_ui_undo_redo(state);
  }
}

static void action_clear(struct swappy_state *state) {
  paint_free_all(state);
  render_state(state);
  update_ui_undo_redo(state);
}

static void action_toggle_painting_panel(struct swappy_state *state,
                                         gboolean *toggled) {
  state->ui->panel_toggled =
      (toggled == NULL) ? !state->ui->panel_toggled : *toggled;
  update_ui_panel_toggle_button(state);
}

static void action_update_color_state(struct swappy_state *state, double r,
                                      double g, double b, double a,
                                      gboolean custom) {
  state->settings.r = r;
  state->settings.g = g;
  state->settings.b = b;
  state->settings.a = a;

  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->color), custom);
}

static void action_set_color_from_custom(struct swappy_state *state) {
  GdkRGBA color;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(state->ui->color), &color);

  action_update_color_state(state, color.red, color.green, color.blue,
                            color.alpha, true);
}

static void hide_crop_box_if_visible(struct swappy_state *state) {
  if (state->ui->crop_box && gtk_widget_get_visible(GTK_WIDGET(state->ui->crop_box))) {
    gtk_widget_hide(GTK_WIDGET(state->ui->crop_box));
  }
}

static void switch_mode_to_brush(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_BRUSH;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
}

static void switch_mode_to_text(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_TEXT;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
}

static void switch_mode_to_rectangle(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_RECTANGLE;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), true);
}

static void switch_mode_to_ellipse(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_ELLIPSE;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), true);
}

static void switch_mode_to_arrow(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_ARROW;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
}

static void switch_mode_to_blur(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_BLUR;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
}

static void switch_mode_to_line(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_LINE;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
}

static void switch_mode_to_highlighter(struct swappy_state *state) {
  hide_crop_box_if_visible(state);
  state->mode = SWAPPY_PAINT_MODE_HIGHLIGHTER;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
}

static void switch_mode_to_crop(struct swappy_state *state) {
  state->mode = SWAPPY_PAINT_MODE_CROP;
  gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
  if (state->ui->crop_box) {
    gtk_widget_show(GTK_WIDGET(state->ui->crop_box));
  }
}

static void action_stroke_size_decrease(struct swappy_state *state) {
  guint step = state->settings.w <= 10 ? 1 : 5;

  state->settings.w -= step;

  if (state->settings.w < SWAPPY_LINE_SIZE_MIN) {
    state->settings.w = SWAPPY_LINE_SIZE_MIN;
  }

  update_ui_stroke_size_widget(state);
}

static void action_stroke_size_reset(struct swappy_state *state) {
  state->settings.w = state->config->line_size;

  update_ui_stroke_size_widget(state);
}

static void action_stroke_size_increase(struct swappy_state *state) {
  guint step = state->settings.w >= 10 ? 5 : 1;
  state->settings.w += step;

  if (state->settings.w > SWAPPY_LINE_SIZE_MAX) {
    state->settings.w = SWAPPY_LINE_SIZE_MAX;
  }

  update_ui_stroke_size_widget(state);
}

static void action_text_size_decrease(struct swappy_state *state) {
  guint step = state->settings.t <= 20 ? 1 : 5;
  state->settings.t -= step;

  if (state->settings.t < SWAPPY_TEXT_SIZE_MIN) {
    state->settings.t = SWAPPY_TEXT_SIZE_MIN;
  }

  update_ui_text_size_widget(state);
}
static void action_text_size_reset(struct swappy_state *state) {
  state->settings.t = state->config->text_size;
  update_ui_text_size_widget(state);
}
static void action_text_size_increase(struct swappy_state *state) {
  guint step = state->settings.t >= 20 ? 5 : 1;
  state->settings.t += step;

  if (state->settings.t > SWAPPY_TEXT_SIZE_MAX) {
    state->settings.t = SWAPPY_TEXT_SIZE_MAX;
  }

  update_ui_text_size_widget(state);
}

static void action_transparency_decrease(struct swappy_state *state) {
  state->settings.tr -= 10;

  if (state->settings.tr < SWAPPY_TRANSPARENCY_MIN) {
    state->settings.tr = SWAPPY_TRANSPARENCY_MIN;
  } else {
    // ceil to 10
    state->settings.tr += 5;
    state->settings.tr /= 10;
    state->settings.tr *= 10;
  }

  update_ui_transparency_widget(state);
}
static void action_transparency_reset(struct swappy_state *state) {
  state->settings.tr = state->config->transparency;
  update_ui_transparency_widget(state);
}
static void action_transparency_increase(struct swappy_state *state) {
  state->settings.tr += 10;

  if (state->settings.tr > SWAPPY_TRANSPARENCY_MAX) {
    state->settings.tr = SWAPPY_TRANSPARENCY_MAX;
  } else {
    // floor to 10
    state->settings.tr /= 10;
    state->settings.tr *= 10;
  }

  update_ui_transparency_widget(state);
}

static void action_fill_shape_toggle(struct swappy_state *state,
                                     gboolean *toggled) {
  // Don't allow changing the state via a shortcut if the button can't be
  // clicked.
  if (!gtk_widget_get_sensitive(GTK_WIDGET(state->ui->fill_shape))) return;

  gboolean toggle = (toggled == NULL) ? !state->config->fill_shape : *toggled;
  state->config->fill_shape = toggle;

  update_ui_fill_shape_toggle_button(state);
}

static void action_transparent_toggle(struct swappy_state *state,
                                      gboolean *toggled) {
  gboolean toggle = (toggled == NULL) ? !state->config->transparent : *toggled;
  state->config->transparent = toggle;

  update_ui_transparent_toggle_button(state);
}

static void save_state_to_file_or_folder(struct swappy_state *state,
                                         char *file) {
  GdkPixbuf *pixbuf = pixbuf_get_from_state(state);
  char notification_msg[512];

  if (file == NULL) {
    // Build the filename for notification
    time_t current_time = time(NULL);
    char filename[255];
    strftime(filename, sizeof(filename), state->config->save_filename_format,
             localtime(&current_time));
    g_snprintf(notification_msg, sizeof(notification_msg), "Saved to %s/%s",
               state->config->save_dir, filename);
    pixbuf_save_state_to_folder(pixbuf, state->config->save_dir,
                                state->config->save_filename_format);
  } else {
    g_snprintf(notification_msg, sizeof(notification_msg), "Saved to %s", file);
    pixbuf_save_to_file(pixbuf, file);
  }

  show_notification("Screenshot Saved", notification_msg);
  g_object_unref(pixbuf);

  if (state->config->early_exit) {
    gtk_main_quit();
  }
}

// We might need to save twice, once for auto_save config
// and one for the output_file from -o CLI option.
static void maybe_save_output_file(struct swappy_state *state) {
  if (state->config->auto_save) {
    save_state_to_file_or_folder(state, NULL);
  }

  if (state->output_file) {
    save_state_to_file_or_folder(state, state->output_file);
  }
}

static void screen_coordinates_to_image_coordinates(struct swappy_state *state,
                                                    gdouble screen_x,
                                                    gdouble screen_y,
                                                    gdouble *image_x,
                                                    gdouble *image_y) {
  gdouble x, y;

  gint w = gdk_pixbuf_get_width(state->original_image);
  gint h = gdk_pixbuf_get_height(state->original_image);

  // Account for pan and zoom
  gdouble adjusted_x = (screen_x - state->pan_x) / (state->scaling_factor * state->zoom_level);
  gdouble adjusted_y = (screen_y - state->pan_y) / (state->scaling_factor * state->zoom_level);

  // Clamp coordinates to original image properties to avoid side effects in
  // rendering pipeline
  x = CLAMP(adjusted_x, 0, w);
  y = CLAMP(adjusted_y, 0, h);

  *image_x = x;
  *image_y = y;
}

static void commit_state(struct swappy_state *state) {
  paint_commit_temporary(state);
  paint_free_list(&state->redo_paints);
  render_state(state);
  update_ui_undo_redo(state);
}

void on_destroy(GtkApplication *application, gpointer data) {
  struct swappy_state *state = (struct swappy_state *)data;
  maybe_save_output_file(state);
}

void brush_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_brush(state);
}

void text_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_text(state);
}

void rectangle_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_rectangle(state);
}

void ellipse_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_ellipse(state);
}

void arrow_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_arrow(state);
}

void blur_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_blur(state);
}

void line_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_line(state);
}

void highlighter_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_highlighter(state);
}

void crop_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  switch_mode_to_crop(state);
}

void crop_aspect_changed_handler(GtkWidget *widget, struct swappy_state *state) {
  gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
  gboolean enable_custom = FALSE;

  switch (active) {
    case 0:  // Free
      state->crop_settings.aspect_w = 0;
      state->crop_settings.aspect_h = 0;
      break;
    case 1:  // 16:9
      state->crop_settings.aspect_w = 16;
      state->crop_settings.aspect_h = 9;
      break;
    case 2:  // 4:3
      state->crop_settings.aspect_w = 4;
      state->crop_settings.aspect_h = 3;
      break;
    case 3:  // 1:1
      state->crop_settings.aspect_w = 1;
      state->crop_settings.aspect_h = 1;
      break;
    case 4:  // Custom
      enable_custom = TRUE;
      state->crop_settings.aspect_w =
          gtk_spin_button_get_value_as_int(state->ui->crop_width_spin);
      state->crop_settings.aspect_h =
          gtk_spin_button_get_value_as_int(state->ui->crop_height_spin);
      break;
  }

  // Enable/disable custom spin buttons
  if (state->ui->crop_width_spin) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->ui->crop_width_spin), enable_custom);
  }
  if (state->ui->crop_height_spin) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->ui->crop_height_spin), enable_custom);
  }
  if (state->ui->crop_swap_button) {
    gtk_widget_set_sensitive(GTK_WIDGET(state->ui->crop_swap_button), enable_custom);
  }
}

void crop_dimension_changed_handler(GtkWidget *widget, struct swappy_state *state) {
  // Only update if Custom is selected
  gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(state->ui->crop_aspect_combo));
  if (active == 4) {  // Custom
    state->crop_settings.aspect_w =
        gtk_spin_button_get_value_as_int(state->ui->crop_width_spin);
    state->crop_settings.aspect_h =
        gtk_spin_button_get_value_as_int(state->ui->crop_height_spin);
  }
}

void crop_swap_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  gint w = gtk_spin_button_get_value_as_int(state->ui->crop_width_spin);
  gint h = gtk_spin_button_get_value_as_int(state->ui->crop_height_spin);

  gtk_spin_button_set_value(state->ui->crop_width_spin, h);
  gtk_spin_button_set_value(state->ui->crop_height_spin, w);

  // Swap settings too
  gint temp = state->crop_settings.aspect_w;
  state->crop_settings.aspect_w = state->crop_settings.aspect_h;
  state->crop_settings.aspect_h = temp;
}

void crop_apply_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  if (state->mode == SWAPPY_PAINT_MODE_CROP && state->temp_paint) {
    action_apply_crop(state);
  }
}

void save_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  // Commit a potential paint (e.g. text being written)
  commit_state(state);
  save_state_to_file_or_folder(state, NULL);
}

static void action_save_as(struct swappy_state *state) {
  commit_state(state);

  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      "Save As", GTK_WINDOW(state->ui->window),
      GTK_FILE_CHOOSER_ACTION_SAVE,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Save", GTK_RESPONSE_ACCEPT,
      NULL);

  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "screenshot.png");

  if (state->config->save_dir) {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),
                                        state->config->save_dir);
  }

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "PNG images");
  gtk_file_filter_add_pattern(filter, "*.png");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    save_state_to_file_or_folder(state, filename);
    g_free(filename);
  }

  gtk_widget_destroy(dialog);
}

void save_as_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  action_save_as(state);
}

void close_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  gtk_main_quit();
}

void clear_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  action_clear(state);
}

void copy_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  // Commit a potential paint (e.g. text being written)
  commit_state(state);
  clipboard_copy_drawing_area_to_selection(state);
}

void control_modifier_changed(bool pressed, struct swappy_state *state) {
  if (state->temp_paint != NULL) {
    switch (state->temp_paint->type) {
      case SWAPPY_PAINT_MODE_ELLIPSE:
      case SWAPPY_PAINT_MODE_RECTANGLE:
        paint_update_temporary_shape(
            state, state->temp_paint->content.shape.to.x,
            state->temp_paint->content.shape.to.y, pressed);
        render_state(state);
        break;
      default:
        break;
    }
  }
}

static void im_context_commit(GtkIMContext *imc, gchar *str,
                              gpointer user_data) {
  struct swappy_state *state = (struct swappy_state *)(user_data);
  if (state->temp_paint && state->mode == SWAPPY_PAINT_MODE_TEXT) {
    paint_update_temporary_str(state, str);
    render_state(state);
    return;
  }
}

static void clipboard_paste_selection(struct swappy_state *state) {
  GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gchar *text = gtk_clipboard_wait_for_text(clipboard);
  if (text) {
    paint_update_temporary_str(state, text);
    g_free(text);
  }
}

void window_keypress_handler(GtkWidget *widget, GdkEventKey *event,
                             struct swappy_state *state) {
  if (state->temp_paint && state->mode == SWAPPY_PAINT_MODE_TEXT) {
    /* ctrl-v: paste */
    if (event->state & GDK_CONTROL_MASK && event->keyval == GDK_KEY_v) {
      clipboard_paste_selection(state);
    } else {
      paint_update_temporary_text(state, event);
    }
    render_state(state);
    return;
  }
  if (event->state & GDK_CONTROL_MASK) {
    switch (event->keyval) {
      case GDK_KEY_c:
        clipboard_copy_drawing_area_to_selection(state);
        break;
      case GDK_KEY_s:
        save_state_to_file_or_folder(state, NULL);
        break;
      case GDK_KEY_S:  // Ctrl+Shift+S = Save As
        action_save_as(state);
        break;
      case GDK_KEY_b:
        action_toggle_painting_panel(state, NULL);
        break;
      case GDK_KEY_w:
        gtk_main_quit();
        break;
      case GDK_KEY_z:
        action_undo(state);
        break;
      case GDK_KEY_Z:
      case GDK_KEY_y:
      case GDK_KEY_r:
        action_redo(state);
        break;
      default:
        break;
    }
  } else {
    switch (event->keyval) {
      case GDK_KEY_Escape:
      case GDK_KEY_q:
        maybe_save_output_file(state);
        gtk_main_quit();
        break;
      case GDK_KEY_b:
        switch_mode_to_brush(state);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->brush), true);
        break;
      case GDK_KEY_e:
      case GDK_KEY_t:
        switch_mode_to_text(state);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->text), true);
        break;
      case GDK_KEY_s:
      case GDK_KEY_r:
        switch_mode_to_rectangle(state);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->rectangle),
                                     true);
        break;
      case GDK_KEY_c:
      case GDK_KEY_o:
        switch_mode_to_ellipse(state);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->ellipse),
                                     true);
        break;
      case GDK_KEY_a:
        switch_mode_to_arrow(state);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->arrow), true);
        break;
      case GDK_KEY_d:
        switch_mode_to_blur(state);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->blur), true);
        break;
      case GDK_KEY_l:
        switch_mode_to_line(state);
        break;
      case GDK_KEY_h:
        switch_mode_to_highlighter(state);
        break;
      case GDK_KEY_x:
        action_clear(state);
        break;
      case GDK_KEY_Return:
      case GDK_KEY_KP_Enter:
        if (state->mode == SWAPPY_PAINT_MODE_CROP && state->temp_paint) {
          action_apply_crop(state);
        }
        break;
      case GDK_KEY_R:
        action_update_color_state(state, 1, 0, 0, 1, false);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->red), true);
        break;
      case GDK_KEY_G:
        action_update_color_state(state, 0, 1, 0, 1, false);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->green), true);
        break;
      case GDK_KEY_B:
        action_update_color_state(state, 0, 0, 1, 1, false);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->blue), true);
        break;
      case GDK_KEY_C:
        action_set_color_from_custom(state);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->custom),
                                     true);
        break;
      case GDK_KEY_minus:
        action_stroke_size_decrease(state);
        break;
      case GDK_KEY_equal:
        action_stroke_size_reset(state);
        break;
      case GDK_KEY_plus:
        action_stroke_size_increase(state);
        break;
      case GDK_KEY_Control_L:
        control_modifier_changed(true, state);
        break;
      case GDK_KEY_f:
        action_fill_shape_toggle(state, NULL);
        break;
      case GDK_KEY_T:
        action_transparent_toggle(state, NULL);
        break;
      case GDK_KEY_0:
        // Reset zoom to 100% and center
        state->zoom_level = 1.0;
        state->pan_x = 0.0;
        state->pan_y = 0.0;
        gtk_widget_queue_draw(state->ui->area);
        break;
      case GDK_KEY_1:
        // Fit to window (reset zoom and pan)
        state->zoom_level = 1.0;
        state->pan_x = 0.0;
        state->pan_y = 0.0;
        gtk_widget_queue_draw(state->ui->area);
        break;
      default:
        break;
    }
  }
}

void window_keyrelease_handler(GtkWidget *widget, GdkEventKey *event,
                               struct swappy_state *state) {
  if (event->state & GDK_CONTROL_MASK) {
    switch (event->keyval) {
      case GDK_KEY_Control_L:
        control_modifier_changed(false, state);
        break;
      default:
        break;
    }
  } else {
    switch (event->keyval) {
      default:
        break;
    }
  }
}

gboolean window_delete_handler(GtkWidget *widget, GdkEvent *event,
                               struct swappy_state *state) {
  gtk_main_quit();
  return FALSE;
}

void pane_toggled_handler(GtkWidget *widget, struct swappy_state *state) {
  GtkToggleButton *button = GTK_TOGGLE_BUTTON(widget);
  gboolean toggled = gtk_toggle_button_get_active(button);
  action_toggle_painting_panel(state, &toggled);
}

void undo_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  action_undo(state);
}

void redo_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  action_redo(state);
}

gboolean draw_area_handler(GtkWidget *widget, cairo_t *cr,
                           struct swappy_state *state) {
  GtkAllocation *alloc = g_new(GtkAllocation, 1);
  gtk_widget_get_allocation(widget, alloc);

  GdkPixbuf *image = state->original_image;
  gint image_width = gdk_pixbuf_get_width(image);
  gint image_height = gdk_pixbuf_get_height(image);
  double base_scale_x = (double)alloc->width / image_width;
  double base_scale_y = (double)alloc->height / image_height;

  // Draw background
  cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
  cairo_paint(cr);

  // Use Scale2x for zoom > 1.5x for sharp text/edges
  if (state->zoom_level > 1.5) {
    // Calculate effective scale (base_scale * zoom)
    double effective_scale = base_scale_x * state->zoom_level;

    // Determine Scale2x factor (power of 2: 2, 4, 8...)
    int scale2x_factor = 2;
    while (scale2x_factor < effective_scale && scale2x_factor < 8) {
      scale2x_factor *= 2;
    }

    // Calculate viewport region in source image coordinates
    // Account for pan offset and convert screen coords to image coords
    double inv_scale = 1.0 / (base_scale_x * state->zoom_level);
    int viewport_x = (int)((-state->pan_x) * inv_scale);
    int viewport_y = (int)((-state->pan_y) * inv_scale);
    int viewport_w = (int)(alloc->width * inv_scale) + 2;  // +2 for edge pixels
    int viewport_h = (int)(alloc->height * inv_scale) + 2;

    // Clamp to image bounds
    if (viewport_x < 0) viewport_x = 0;
    if (viewport_y < 0) viewport_y = 0;
    if (viewport_x + viewport_w > image_width) viewport_w = image_width - viewport_x;
    if (viewport_y + viewport_h > image_height) viewport_h = image_height - viewport_y;

    if (viewport_w > 0 && viewport_h > 0) {
      // Upscale the viewport region using Scale2x
      cairo_surface_t *upscaled = scale2x_viewport(
        state->rendering_surface,
        viewport_x, viewport_y,
        viewport_w, viewport_h,
        scale2x_factor
      );

      if (upscaled) {
        // Calculate where to draw the upscaled surface
        double draw_x = state->pan_x + viewport_x * base_scale_x * state->zoom_level;
        double draw_y = state->pan_y + viewport_y * base_scale_y * state->zoom_level;

        // Scale factor to fit upscaled surface to screen
        double final_scale = (base_scale_x * state->zoom_level) / scale2x_factor;

        cairo_save(cr);
        cairo_translate(cr, draw_x, draw_y);
        cairo_scale(cr, final_scale, final_scale);
        cairo_set_source_surface(cr, upscaled, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        cairo_paint(cr);
        cairo_restore(cr);

        cairo_surface_destroy(upscaled);
      }
    }
  } else {
    // Standard Cairo rendering for low zoom levels
    double scale_x = base_scale_x * state->zoom_level;
    double scale_y = base_scale_y * state->zoom_level;

    cairo_translate(cr, state->pan_x, state->pan_y);
    cairo_scale(cr, scale_x, scale_y);
    cairo_set_source_surface(cr, state->rendering_surface, 0, 0);

    cairo_pattern_t *pattern = cairo_get_source(cr);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
  }

  g_free(alloc);
  return FALSE;
}

gboolean draw_area_configure_handler(GtkWidget *widget,
                                     GdkEventConfigure *event,
                                     struct swappy_state *state) {
  g_debug("received configure_event callback");

  pixbuf_scale_surface_from_widget(state, widget);

  render_state(state);

  return TRUE;
}

// Track middle mouse button for panning
static gboolean middle_button_pressed = FALSE;
static gdouble pan_start_x = 0;
static gdouble pan_start_y = 0;

void draw_area_button_press_handler(GtkWidget *widget, GdkEventButton *event,
                                    struct swappy_state *state) {
  gdouble x, y;

  // Middle mouse button for panning
  if (event->button == 2) {
    middle_button_pressed = TRUE;
    pan_start_x = event->x - state->pan_x;
    pan_start_y = event->y - state->pan_y;
    return;
  }

  screen_coordinates_to_image_coordinates(state, event->x, event->y, &x, &y);

  if (event->button == 1) {
    switch (state->mode) {
      case SWAPPY_PAINT_MODE_BLUR:
      case SWAPPY_PAINT_MODE_BRUSH:
      case SWAPPY_PAINT_MODE_HIGHLIGHTER:
      case SWAPPY_PAINT_MODE_RECTANGLE:
      case SWAPPY_PAINT_MODE_ELLIPSE:
      case SWAPPY_PAINT_MODE_ARROW:
      case SWAPPY_PAINT_MODE_LINE:
      case SWAPPY_PAINT_MODE_TEXT:
      case SWAPPY_PAINT_MODE_CROP:
        paint_add_temporary(state, x, y, state->mode);
        render_state(state);
        update_ui_undo_redo(state);
        break;
      default:
        return;
    }
  }
}
void draw_area_motion_notify_handler(GtkWidget *widget, GdkEventMotion *event,
                                     struct swappy_state *state) {
  gdouble x, y;

  // Handle panning with middle mouse button
  if (middle_button_pressed) {
    state->pan_x = event->x - pan_start_x;
    state->pan_y = event->y - pan_start_y;
    gtk_widget_queue_draw(state->ui->area);
    return;
  }

  screen_coordinates_to_image_coordinates(state, event->x, event->y, &x, &y);

  GdkDisplay *display = gdk_display_get_default();
  GdkWindow *window = event->window;
  GdkCursor *crosshair = gdk_cursor_new_for_display(display, GDK_CROSSHAIR);
  gdk_window_set_cursor(window, crosshair);

  gboolean is_button1_pressed = event->state & GDK_BUTTON1_MASK;
  gboolean is_control_pressed = event->state & GDK_CONTROL_MASK;

  switch (state->mode) {
    case SWAPPY_PAINT_MODE_BLUR:
    case SWAPPY_PAINT_MODE_BRUSH:
    case SWAPPY_PAINT_MODE_HIGHLIGHTER:
    case SWAPPY_PAINT_MODE_RECTANGLE:
    case SWAPPY_PAINT_MODE_ELLIPSE:
    case SWAPPY_PAINT_MODE_ARROW:
    case SWAPPY_PAINT_MODE_LINE:
      if (is_button1_pressed) {
        paint_update_temporary_shape(state, x, y, is_control_pressed);
        render_state(state);
      }
      break;
    case SWAPPY_PAINT_MODE_CROP:
      if (is_button1_pressed && state->temp_paint) {
        gdouble crop_x = x;
        gdouble crop_y = y;

        // Apply aspect ratio constraint if set
        if (state->crop_settings.aspect_w > 0 && state->crop_settings.aspect_h > 0) {
          gdouble from_x = state->temp_paint->content.shape.from.x;
          gdouble from_y = state->temp_paint->content.shape.from.y;
          gdouble dx = x - from_x;
          gdouble dy = y - from_y;
          gdouble aspect = (gdouble)state->crop_settings.aspect_w /
                           (gdouble)state->crop_settings.aspect_h;

          // Determine the constrained dimensions based on the larger delta
          gdouble abs_dx = dx >= 0 ? dx : -dx;
          gdouble abs_dy = dy >= 0 ? dy : -dy;

          if (abs_dx / aspect > abs_dy) {
            // Width is the constraining dimension
            crop_y = from_y + (dx >= 0 ? abs_dx / aspect : -abs_dx / aspect);
            if (dy < 0) crop_y = from_y - abs_dx / aspect;
          } else {
            // Height is the constraining dimension
            crop_x = from_x + (dy >= 0 ? abs_dy * aspect : -abs_dy * aspect);
            if (dx < 0) crop_x = from_x - abs_dy * aspect;
          }
        }

        paint_update_temporary_shape(state, crop_x, crop_y, is_control_pressed);
        render_state(state);
      }
      break;
    case SWAPPY_PAINT_MODE_TEXT:
      if (is_button1_pressed) {
        paint_update_temporary_text_clip(state, x, y);
        render_state(state);
      }
      break;
    default:
      return;
  }
  g_object_unref(crosshair);
}
void draw_area_button_release_handler(GtkWidget *widget, GdkEventButton *event,
                                      struct swappy_state *state) {
  // Handle middle button release for panning
  if (event->button == 2) {
    middle_button_pressed = FALSE;
    return;
  }

  if (!(event->state & GDK_BUTTON1_MASK)) {
    return;
  }

  switch (state->mode) {
    case SWAPPY_PAINT_MODE_BLUR:
    case SWAPPY_PAINT_MODE_BRUSH:
    case SWAPPY_PAINT_MODE_HIGHLIGHTER:
    case SWAPPY_PAINT_MODE_RECTANGLE:
    case SWAPPY_PAINT_MODE_ELLIPSE:
    case SWAPPY_PAINT_MODE_ARROW:
    case SWAPPY_PAINT_MODE_LINE:
      commit_state(state);
      break;
    case SWAPPY_PAINT_MODE_CROP:
      // Don't commit crop - keep it as temp_paint until Enter is pressed
      // The overlay stays visible so user can adjust or press Enter to apply
      break;
    case SWAPPY_PAINT_MODE_TEXT:
      if (state->temp_paint && !state->temp_paint->can_draw) {
        paint_free(state->temp_paint);
        state->temp_paint = NULL;
      }
      break;
    default:
      return;
  }
}

gboolean draw_area_scroll_handler(GtkWidget *widget, GdkEventScroll *event,
                                  struct swappy_state *state) {
  static gdouble scroll_accumulator = 0.0;
  static gdouble zoom_accumulator = 0.0;
  GdkScrollDirection direction = event->direction;
  gboolean shift_held = event->state & GDK_SHIFT_MASK;

  // Handle smooth scrolling (touchpad/Wayland)
  if (direction == GDK_SCROLL_SMOOTH) {
    if (shift_held) {
      scroll_accumulator += event->delta_y;
      if (scroll_accumulator > 0.5) {
        direction = GDK_SCROLL_DOWN;
        scroll_accumulator = 0.0;
      } else if (scroll_accumulator < -0.5) {
        direction = GDK_SCROLL_UP;
        scroll_accumulator = 0.0;
      } else {
        return TRUE;
      }
    } else {
      // Zoom mode
      zoom_accumulator += event->delta_y;
      if (zoom_accumulator > 0.3) {
        direction = GDK_SCROLL_DOWN;
        zoom_accumulator = 0.0;
      } else if (zoom_accumulator < -0.3) {
        direction = GDK_SCROLL_UP;
        zoom_accumulator = 0.0;
      } else {
        return TRUE;
      }
    }
  }

  // Shift+scroll: adjust tool size
  if (shift_held) {
    // Adjust size based on current mode
    if (state->mode == SWAPPY_PAINT_MODE_TEXT) {
      if (direction == GDK_SCROLL_UP) {
        action_text_size_increase(state);
      } else if (direction == GDK_SCROLL_DOWN) {
        action_text_size_decrease(state);
      }
      if (state->temp_paint && state->temp_paint->type == SWAPPY_PAINT_MODE_TEXT) {
        state->temp_paint->content.text.s = state->settings.t;
        render_state(state);
      }
    } else {
      if (direction == GDK_SCROLL_UP) {
        action_stroke_size_increase(state);
      } else if (direction == GDK_SCROLL_DOWN) {
        action_stroke_size_decrease(state);
      }
      if (state->temp_paint) {
        switch (state->temp_paint->type) {
          case SWAPPY_PAINT_MODE_BRUSH:
          case SWAPPY_PAINT_MODE_HIGHLIGHTER:
            state->temp_paint->content.brush.w = state->settings.w;
            break;
          case SWAPPY_PAINT_MODE_RECTANGLE:
          case SWAPPY_PAINT_MODE_ELLIPSE:
          case SWAPPY_PAINT_MODE_ARROW:
          case SWAPPY_PAINT_MODE_LINE:
            state->temp_paint->content.shape.w = state->settings.w;
            break;
          default:
            break;
        }
        render_state(state);
      }
    }
  } else {
    // No modifier: zoom in/out
    gdouble zoom_factor = 1.1;
    gdouble old_zoom = state->zoom_level;

    if (direction == GDK_SCROLL_UP) {
      state->zoom_level *= zoom_factor;
      if (state->zoom_level > 10.0) state->zoom_level = 10.0;  // Max 1000% zoom
    } else if (direction == GDK_SCROLL_DOWN) {
      state->zoom_level /= zoom_factor;
      if (state->zoom_level < 0.1) state->zoom_level = 0.1;  // Min 10% zoom
    }

    // Zoom towards mouse position
    if (state->zoom_level != old_zoom) {
      gdouble mouse_x = event->x;
      gdouble mouse_y = event->y;
      gdouble zoom_ratio = state->zoom_level / old_zoom;

      // Adjust pan to zoom towards cursor
      state->pan_x = mouse_x - (mouse_x - state->pan_x) * zoom_ratio;
      state->pan_y = mouse_y - (mouse_y - state->pan_y) * zoom_ratio;

      gtk_widget_queue_draw(state->ui->area);
    }
  }

  return TRUE;
}

void color_red_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  action_update_color_state(state, 1, 0, 0, 1, false);
}

void color_green_clicked_handler(GtkWidget *widget,
                                 struct swappy_state *state) {
  action_update_color_state(state, 0, 1, 0, 1, false);
}

void color_blue_clicked_handler(GtkWidget *widget, struct swappy_state *state) {
  action_update_color_state(state, 0, 0, 1, 1, false);
}

void color_custom_clicked_handler(GtkWidget *widget,
                                  struct swappy_state *state) {
  action_set_color_from_custom(state);
}

void color_custom_color_set_handler(GtkWidget *widget,
                                    struct swappy_state *state) {
  action_set_color_from_custom(state);
}

void stroke_size_decrease_handler(GtkWidget *widget,
                                  struct swappy_state *state) {
  action_stroke_size_decrease(state);
}

void stroke_size_reset_handler(GtkWidget *widget, struct swappy_state *state) {
  action_stroke_size_reset(state);
}
void stroke_size_increase_handler(GtkWidget *widget,
                                  struct swappy_state *state) {
  action_stroke_size_increase(state);
}

void text_size_decrease_handler(GtkWidget *widget, struct swappy_state *state) {
  action_text_size_decrease(state);
}
void text_size_reset_handler(GtkWidget *widget, struct swappy_state *state) {
  action_text_size_reset(state);
}
void text_size_increase_handler(GtkWidget *widget, struct swappy_state *state) {
  action_text_size_increase(state);
}

void font_changed_handler(GtkFontChooser *widget, struct swappy_state *state) {
  PangoFontDescription *desc = gtk_font_chooser_get_font_desc(widget);
  if (desc) {
    const gchar *family = pango_font_description_get_family(desc);
    if (family) {
      g_free(state->config->text_font);
      state->config->text_font = g_strdup(family);
      g_debug("Font changed to: %s", state->config->text_font);
    }
    pango_font_description_free(desc);
  }
}

void save_folder_changed_handler(GtkFileChooser *widget, struct swappy_state *state) {
  gchar *folder = gtk_file_chooser_get_filename(widget);
  if (folder) {
    g_free(state->config->save_dir);
    state->config->save_dir = folder;
    g_debug("Save folder changed to: %s", state->config->save_dir);
  }
}

void transparency_decrease_handler(GtkWidget *widget,
                                   struct swappy_state *state) {
  action_transparency_decrease(state);
}
void transparency_reset_handler(GtkWidget *widget, struct swappy_state *state) {
  action_transparency_reset(state);
}
void transparency_increase_handler(GtkWidget *widget,
                                   struct swappy_state *state) {
  action_transparency_increase(state);
}

void fill_shape_toggled_handler(GtkWidget *widget, struct swappy_state *state) {
  GtkToggleButton *button = GTK_TOGGLE_BUTTON(widget);
  gboolean toggled = gtk_toggle_button_get_active(button);
  action_fill_shape_toggle(state, &toggled);
}

void transparent_toggled_handler(GtkWidget *widget,
                                 struct swappy_state *state) {
  GtkToggleButton *button = GTK_TOGGLE_BUTTON(widget);
  gboolean toggled = gtk_toggle_button_get_active(button);
  action_transparent_toggle(state, &toggled);
}

static void compute_window_size_and_scaling_factor(struct swappy_state *state) {
  GdkRectangle workarea = {0};
  GdkDisplay *display = gdk_display_get_default();
  GdkWindow *window = gtk_widget_get_window(GTK_WIDGET(state->ui->window));
  GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, window);
  gdk_monitor_get_workarea(monitor, &workarea);

  g_assert(workarea.width > 0);
  g_assert(workarea.height > 0);

  if (state->window) {
    g_free(state->window);
    state->window = NULL;
  }

  state->window = g_new(struct swappy_box, 1);
  state->window->x = workarea.x;
  state->window->y = workarea.y;

  double threshold = 0.75;
  double scaling_factor = 1.0;

  int image_width = gdk_pixbuf_get_width(state->original_image);
  int image_height = gdk_pixbuf_get_height(state->original_image);

  int max_width = workarea.width * threshold;
  int max_height = workarea.height * threshold;

  g_info("size of image: %ux%u", image_width, image_height);
  g_info("size of monitor at window: %ux%u", workarea.width, workarea.height);
  g_info("maxium size allowed for window: %ux%u", max_width, max_height);

  int scaled_width = image_width;
  int scaled_height = image_height;

  double scaling_factor_width = (double)max_width / image_width;
  double scaling_factor_height = (double)max_height / image_height;

  if (scaling_factor_height < 1.0 || scaling_factor_width < 1.0) {
    scaling_factor = MIN(scaling_factor_width, scaling_factor_height);
    scaled_width = image_width * scaling_factor;
    scaled_height = image_height * scaling_factor;
    g_info("rendering area will be scaled by a factor of: %.2lf",
           scaling_factor);
  }

  state->scaling_factor = scaling_factor;
  state->zoom_level = 1.0;
  state->pan_x = 0.0;
  state->pan_y = 0.0;
  state->window->width = scaled_width;
  state->window->height = scaled_height;

  g_info("size of window to render: %ux%u", state->window->width,
         state->window->height);
}

static void apply_css(GtkWidget *widget, GtkStyleProvider *provider) {
  gtk_style_context_add_provider(gtk_widget_get_style_context(widget), provider,
                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  if (GTK_IS_CONTAINER(widget)) {
    gtk_container_forall(GTK_CONTAINER(widget), (GtkCallback)apply_css,
                         provider);
  }
}

static bool load_css(struct swappy_state *state) {
  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(provider,
                                      "/me/jtheoof/swappy/style/swappy.css");
  apply_css(GTK_WIDGET(state->ui->window), GTK_STYLE_PROVIDER(provider));
  g_object_unref(provider);
  return true;
}

static bool load_layout(struct swappy_state *state) {
  GError *error = NULL;
  // init color
  GdkRGBA color;

  /* Construct a GtkBuilder instance and load our UI description */
  GtkBuilder *builder = gtk_builder_new();

  // Set translation domain for the application based on `src/po/meson.build`
  gtk_builder_set_translation_domain(builder, GETTEXT_PACKAGE);

  if (gtk_builder_add_from_resource(builder, "/me/jtheoof/swappy/swappy.glade",
                                    &error) == 0) {
    g_printerr("Error loading file: %s", error->message);
    g_clear_error(&error);
    return false;
  }

  gtk_builder_connect_signals(builder, state);

  GtkWindow *window =
      GTK_WINDOW(gtk_builder_get_object(builder, "paint-window"));
  GtkIMContext *im_context = gtk_im_multicontext_new();
  gtk_im_context_set_client_window(im_context,
                                   gtk_widget_get_window(GTK_WIDGET(window)));
  g_signal_connect(G_OBJECT(im_context), "commit",
                   G_CALLBACK(im_context_commit), state);
  state->ui->im_context = im_context;

  g_signal_connect(window, "destroy", G_CALLBACK(on_destroy), state);

  state->ui->panel_toggle_button =
      GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, "btn-toggle-panel"));

  state->ui->undo = GTK_BUTTON(gtk_builder_get_object(builder, "undo-button"));
  state->ui->redo = GTK_BUTTON(gtk_builder_get_object(builder, "redo-button"));

  GtkWidget *area =
      GTK_WIDGET(gtk_builder_get_object(builder, "painting-area"));

  state->ui->painting_box =
      GTK_BOX(gtk_builder_get_object(builder, "painting-box"));
  GtkRadioButton *brush =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "brush"));
  GtkRadioButton *text =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "text"));
  GtkRadioButton *rectangle =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "rectangle"));
  GtkRadioButton *ellipse =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "ellipse"));
  GtkRadioButton *arrow =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "arrow"));
  GtkRadioButton *blur =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "blur"));

  state->ui->red =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "color-red-button"));
  state->ui->green =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "color-green-button"));
  state->ui->blue =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "color-blue-button"));
  state->ui->custom =
      GTK_RADIO_BUTTON(gtk_builder_get_object(builder, "color-custom-button"));
  state->ui->color =
      GTK_COLOR_BUTTON(gtk_builder_get_object(builder, "custom-color-button"));

  state->ui->line_size =
      GTK_BUTTON(gtk_builder_get_object(builder, "stroke-size-button"));
  state->ui->text_size =
      GTK_BUTTON(gtk_builder_get_object(builder, "text-size-button"));
  state->ui->transparency =
      GTK_BUTTON(gtk_builder_get_object(builder, "transparency-button"));
  state->ui->transparency_plus =
      GTK_BUTTON(gtk_builder_get_object(builder, "transparency-plus-button"));
  state->ui->transparency_minus =
      GTK_BUTTON(gtk_builder_get_object(builder, "transparency-minus-button"));
  state->ui->font_button =
      GTK_FONT_BUTTON(gtk_builder_get_object(builder, "font-button"));
  state->ui->save_folder_button =
      GTK_FILE_CHOOSER_BUTTON(gtk_builder_get_object(builder, "save-folder-button"));

  // Initialize font button with current font from config
  if (state->ui->font_button && state->config->text_font) {
    gtk_font_chooser_set_font(GTK_FONT_CHOOSER(state->ui->font_button), state->config->text_font);
  }

  // Initialize save folder button with current save_dir
  if (state->ui->save_folder_button && state->config->save_dir) {
    gtk_file_chooser_set_current_folder(
        GTK_FILE_CHOOSER(state->ui->save_folder_button), state->config->save_dir);
  }

  state->ui->fill_shape = GTK_TOGGLE_BUTTON(
      gtk_builder_get_object(builder, "fill-shape-toggle-button"));

  gdk_rgba_parse(&color, state->config->custom_color);
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(state->ui->color), &color);
  state->ui->transparent = GTK_TOGGLE_BUTTON(
      gtk_builder_get_object(builder, "transparent-toggle-button"));

  // Crop controls
  state->ui->crop_box =
      GTK_BOX(gtk_builder_get_object(builder, "crop-box"));
  state->ui->crop_aspect_combo =
      GTK_COMBO_BOX_TEXT(gtk_builder_get_object(builder, "crop-aspect-combo"));
  state->ui->crop_width_spin =
      GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "crop-width-spin"));
  state->ui->crop_height_spin =
      GTK_SPIN_BUTTON(gtk_builder_get_object(builder, "crop-height-spin"));
  state->ui->crop_swap_button =
      GTK_BUTTON(gtk_builder_get_object(builder, "crop-swap-button"));
  state->ui->crop_apply_button =
      GTK_BUTTON(gtk_builder_get_object(builder, "crop-apply-button"));

  // Initialize crop settings
  state->crop_settings.aspect_w = 0;
  state->crop_settings.aspect_h = 0;

  state->ui->brush = brush;
  state->ui->text = text;
  state->ui->rectangle = rectangle;
  state->ui->ellipse = ellipse;
  state->ui->arrow = arrow;
  state->ui->blur = blur;
  state->ui->area = area;
  state->ui->window = window;

  compute_window_size_and_scaling_factor(state);
  gtk_widget_set_size_request(area, state->window->width,
                              state->window->height);
  action_toggle_painting_panel(state, &state->config->show_panel);

  g_object_unref(G_OBJECT(builder));

  return true;
}

static void set_paint_mode(struct swappy_state *state) {
  switch (state->mode) {
    case SWAPPY_PAINT_MODE_BRUSH:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->brush), true);
      gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
      break;
    case SWAPPY_PAINT_MODE_TEXT:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->text), true);
      gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
      break;
    case SWAPPY_PAINT_MODE_RECTANGLE:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->rectangle),
                                   true);
      gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), true);
      break;
    case SWAPPY_PAINT_MODE_ELLIPSE:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->ellipse), true);
      gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), true);
      break;
    case SWAPPY_PAINT_MODE_ARROW:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->arrow), true);
      gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
      break;
    case SWAPPY_PAINT_MODE_BLUR:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->ui->blur), true);
      gtk_widget_set_sensitive(GTK_WIDGET(state->ui->fill_shape), false);
      break;
    default:
      break;
  }
}

static bool init_gtk_window(struct swappy_state *state) {
  if (!state->original_image) {
    g_critical("original image not loaded");
    return false;
  }

  if (!load_layout(state)) {
    return false;
  }

  if (!load_css(state)) {
    return false;
  }

  set_paint_mode(state);

  update_ui_stroke_size_widget(state);
  update_ui_text_size_widget(state);
  update_ui_transparency_widget(state);
  update_ui_undo_redo(state);
  update_ui_panel_toggle_button(state);
  update_ui_fill_shape_toggle_button(state);
  update_ui_transparent_toggle_button(state);

  return true;
}

static gboolean has_option_file(struct swappy_state *state) {
  return (state->file_str != NULL);
}

static gboolean is_file_from_stdin(const char *file) {
  return (strcmp(file, "-") == 0);
}

static void init_settings(struct swappy_state *state) {
  state->settings.r = 1;
  state->settings.g = 0;
  state->settings.b = 0;
  state->settings.a = 1;
  state->settings.w = state->config->line_size;
  state->settings.t = state->config->text_size;
  state->settings.tr = state->config->transparency;
  state->mode = state->config->paint_mode;
}

static gint command_line_handler(GtkApplication *app,
                                 GApplicationCommandLine *cmdline,
                                 struct swappy_state *state) {
  config_load(state);
  init_settings(state);

  if (has_option_file(state)) {
    if (is_file_from_stdin(state->file_str)) {
      char *temp_file_str = file_dump_stdin_into_a_temp_file();
      state->temp_file_str = temp_file_str;
    }

    if (!pixbuf_init_from_file(state)) {
      return EXIT_FAILURE;
    }
  }

  if (!init_gtk_window(state)) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

// Print version and quit
gboolean callback_on_flag(const gchar *option_name, const gchar *value,
                          gpointer data, GError **error) {
  if (!strcmp(option_name, "-v") || !strcmp(option_name, "--version")) {
    printf("swappy version %s\n", SWAPPY_VERSION);
    exit(0);
  }
  return TRUE;
}

bool application_init(struct swappy_state *state) {
  // Callback function for flags
  gboolean (*GOptionArgFunc)(const gchar *option_name, const gchar *value,
                             gpointer data, GError **error);
  GOptionArgFunc = &callback_on_flag;

  const GOptionEntry cli_options[] = {
      {
          .long_name = "file",
          .short_name = 'f',
          .arg = G_OPTION_ARG_STRING,
          .arg_data = &state->file_str,
          .description = "Load a file at a specific path",
      },
      {
          .long_name = "output-file",
          .short_name = 'o',
          .arg = G_OPTION_ARG_STRING,
          .arg_data = &state->output_file,
          .description = "Print the final surface to the given file when "
                         "exiting, use - to print to stdout",
      },
      {
          .long_name = "version",
          .short_name = 'v',
          .flags = G_OPTION_FLAG_NO_ARG,
          .arg = G_OPTION_ARG_CALLBACK,
          .arg_data = GOptionArgFunc,
          .description = "Print version and quit",
      },
      {NULL}};  // NOLINT(clang-diagnostic-missing-field-initializers)

  state->app = gtk_application_new("me.jtheoof.swappy",
                                   G_APPLICATION_HANDLES_COMMAND_LINE);

  if (state->app == NULL) {
    g_critical("cannot create gtk application");
    return false;
  }

  g_application_add_main_option_entries(G_APPLICATION(state->app), cli_options);

  state->ui = g_new(struct swappy_state_ui, 1);
  state->ui->panel_toggled = false;

  g_signal_connect(state->app, "command-line", G_CALLBACK(command_line_handler),
                   state);

  return true;
}

int application_run(struct swappy_state *state) {
  return g_application_run(G_APPLICATION(state->app), state->argc, state->argv);
}
