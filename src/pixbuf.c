#include "pixbuf.h"

#include <cairo/cairo.h>
#include <gio/gio.h>
#include <gio/gunixoutputstream.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "enhance.h"

static void write_file(GdkPixbuf *pixbuf, char *path);

/* Data passed to async upscale thread */
typedef struct {
  gchar *upscale_command;
  gchar *in_path;
  gchar *out_path;
} UpscaleTaskData;

static void upscale_task_data_free(UpscaleTaskData *data) {
  if (data->in_path) {
    g_unlink(data->in_path);
    g_free(data->in_path);
  }
  if (data->out_path) {
    g_unlink(data->out_path);
    g_free(data->out_path);
  }
  g_free(data->upscale_command);
  g_free(data);
}

/*
 * Flatten a surface by compositing it over the preview background color.
 * This ensures saved images match what users see in the preview.
 */
static cairo_surface_t *flatten_surface(cairo_surface_t *src, int width, int height) {
  cairo_surface_t *flat = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
  if (cairo_surface_status(flat) != CAIRO_STATUS_SUCCESS) {
    return NULL;
  }

  cairo_t *cr = cairo_create(flat);

  /* Fill with the same background color used in preview (0.2, 0.2, 0.2) */
  cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
  cairo_paint(cr);

  /* Composite the source surface over the background */
  cairo_set_source_surface(cr, src, 0, 0);
  cairo_paint(cr);

  cairo_destroy(cr);
  return flat;
}

static gchar *replace_token(const gchar *source, const gchar *token,
                            const gchar *replacement) {
  gsize token_len = strlen(token);
  const gchar *cursor = source;
  const gchar *match = NULL;
  GString *out = g_string_new(NULL);

  while ((match = g_strstr_len(cursor, -1, token)) != NULL) {
    g_string_append_len(out, cursor, match - cursor);
    g_string_append(out, replacement);
    cursor = match + token_len;
  }

  g_string_append(out, cursor);
  return g_string_free(out, FALSE);
}

GdkPixbuf *pixbuf_apply_upscale_command(struct swappy_state *state,
                                        GdkPixbuf *pixbuf) {
  const gchar *template = state->config->upscale_command;
  if (!template || template[0] == '\0') {
    return NULL;
  }

  if (!g_strstr_len(template, -1, "%INPUT%") ||
      !g_strstr_len(template, -1, "%OUTPUT%")) {
    g_warning("upscale_command must contain both %%INPUT%% and %%OUTPUT%% placeholders");
    return NULL;
  }

  gchar in_name[] = "swappy-upscale-input-XXXXXX.png";
  gchar out_name[] = "swappy-upscale-output-XXXXXX.png";
  gchar *in_path = g_build_filename(g_get_tmp_dir(), in_name, NULL);
  gchar *out_path = g_build_filename(g_get_tmp_dir(), out_name, NULL);
  gint in_fd = g_mkstemp(in_path);
  gint out_fd = g_mkstemp(out_path);
  gchar *cmd_with_input = NULL;
  gchar *command = NULL;
  GError *error = NULL;
  gint status = 0;
  GdkPixbuf *upscaled = NULL;

  if (in_fd == -1 || out_fd == -1) {
    g_warning("unable to create temporary files for upscaling");
    goto cleanup;
  }

  close(in_fd);
  close(out_fd);
  write_file(pixbuf, in_path);

  cmd_with_input = replace_token(template, "%INPUT%", in_path);
  command = replace_token(cmd_with_input, "%OUTPUT%", out_path);

  if (!g_spawn_command_line_sync(command, NULL, NULL, &status, &error)) {
    g_warning("failed to execute upscale_command: %s", error->message);
    g_clear_error(&error);
    goto cleanup;
  }

  if (!g_spawn_check_wait_status(status, &error)) {
    g_warning("upscale_command returned a non-zero status: %s", error->message);
    g_clear_error(&error);
    goto cleanup;
  }

  if (!g_file_test(out_path, G_FILE_TEST_EXISTS)) {
    g_warning("upscale_command did not create output file: %s", out_path);
    goto cleanup;
  }

  upscaled = gdk_pixbuf_new_from_file(out_path, &error);
  if (!upscaled) {
    g_warning("unable to read upscaled output file: %s", error->message);
    g_clear_error(&error);
  } else {
    g_info("upscale_command applied successfully");
  }

cleanup:
  if (in_path) {
    g_unlink(in_path);
  }
  if (out_path) {
    g_unlink(out_path);
  }
  g_free(in_path);
  g_free(out_path);
  g_free(cmd_with_input);
  g_free(command);
  return upscaled;
}

/* Thread function for async upscaling */
static void upscale_thread_func(GTask *task, gpointer source_object,
                                gpointer task_data, GCancellable *cancellable) {
  UpscaleTaskData *data = task_data;
  GError *error = NULL;
  gint status = 0;
  GdkPixbuf *upscaled = NULL;

  if (g_task_return_error_if_cancelled(task)) {
    return;
  }

  if (!g_spawn_command_line_sync(data->upscale_command, NULL, NULL, &status, &error)) {
    g_task_return_error(task, error);
    return;
  }

  if (!g_spawn_check_wait_status(status, &error)) {
    g_task_return_error(task, error);
    return;
  }

  if (!g_file_test(data->out_path, G_FILE_TEST_EXISTS)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "upscale_command did not create output file: %s", data->out_path);
    return;
  }

  upscaled = gdk_pixbuf_new_from_file(data->out_path, &error);
  if (!upscaled) {
    g_task_return_error(task, error);
    return;
  }

  g_info("async upscale_command completed successfully");
  g_task_return_pointer(task, upscaled, g_object_unref);
}

void pixbuf_apply_upscale_command_async(struct swappy_state *state,
                                        GdkPixbuf *pixbuf,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  const gchar *template = state->config->upscale_command;
  if (!template || template[0] == '\0') {
    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    g_task_return_pointer(task, NULL, NULL);
    g_object_unref(task);
    return;
  }

  if (!g_strstr_len(template, -1, "%INPUT%") ||
      !g_strstr_len(template, -1, "%OUTPUT%")) {
    g_warning("upscale_command must contain both %%INPUT%% and %%OUTPUT%% placeholders");
    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    g_task_return_pointer(task, NULL, NULL);
    g_object_unref(task);
    return;
  }

  gchar in_name[] = "swappy-upscale-input-XXXXXX.png";
  gchar out_name[] = "swappy-upscale-output-XXXXXX.png";
  gchar *in_path = g_build_filename(g_get_tmp_dir(), in_name, NULL);
  gchar *out_path = g_build_filename(g_get_tmp_dir(), out_name, NULL);
  gint in_fd = g_mkstemp(in_path);
  gint out_fd = g_mkstemp(out_path);

  if (in_fd == -1 || out_fd == -1) {
    g_warning("unable to create temporary files for async upscaling");
    g_free(in_path);
    g_free(out_path);
    GTask *task = g_task_new(NULL, NULL, callback, user_data);
    g_task_return_pointer(task, NULL, NULL);
    g_object_unref(task);
    return;
  }

  close(in_fd);
  close(out_fd);
  write_file(pixbuf, in_path);

  gchar *cmd_with_input = replace_token(template, "%INPUT%", in_path);
  gchar *command = replace_token(cmd_with_input, "%OUTPUT%", out_path);
  g_free(cmd_with_input);

  UpscaleTaskData *data = g_new0(UpscaleTaskData, 1);
  data->upscale_command = command;
  data->in_path = in_path;
  data->out_path = out_path;

  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_task_set_task_data(task, data, (GDestroyNotify)upscale_task_data_free);
  g_task_run_in_thread(task, upscale_thread_func);
  g_object_unref(task);

  g_info("async upscale started");
}

GdkPixbuf *pixbuf_apply_upscale_command_finish(GAsyncResult *result,
                                               GError **error) {
  return g_task_propagate_pointer(G_TASK(result), error);
}

GdkPixbuf *pixbuf_get_from_state(struct swappy_state *state) {
  guint width = cairo_image_surface_get_width(state->rendering_surface);
  guint height = cairo_image_surface_get_height(state->rendering_surface);

  cairo_surface_t *surface_to_save = state->rendering_surface;
  cairo_surface_t *enhanced = NULL;
  cairo_surface_t *flattened = NULL;

  /* Apply image enhancement if configured */
  EnhancePreset preset = (EnhancePreset)state->config->enhance_preset;
  if (preset != ENHANCE_NONE) {
    enhanced = enhance_surface(state->rendering_surface, preset);
    if (enhanced && cairo_surface_status(enhanced) == CAIRO_STATUS_SUCCESS) {
      surface_to_save = enhanced;
      g_info("Applied enhancement preset: %s", enhance_preset_name(preset));
    }
  }

  /* Flatten the surface (composite over background) so saved image matches preview */
  flattened = flatten_surface(surface_to_save, width, height);
  if (flattened && cairo_surface_status(flattened) == CAIRO_STATUS_SUCCESS) {
    surface_to_save = flattened;
  }

  GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface_to_save, 0, 0, width, height);

  if (flattened) {
    cairo_surface_destroy(flattened);
  }
  if (enhanced) {
    cairo_surface_destroy(enhanced);
  }

  /* Reuse cached upscaled pixbuf if available (from async preview) */
  if (state->upscaled_pixbuf_cache) {
    g_object_unref(pixbuf);
    pixbuf = g_object_ref(state->upscaled_pixbuf_cache);
    g_info("reusing cached upscaled pixbuf for save");
  } else {
    /* Fallback to sync upscale if no cache (blocking but only on save) */
    GdkPixbuf *upscaled = pixbuf_apply_upscale_command(state, pixbuf);
    if (upscaled) {
      g_object_unref(pixbuf);
      pixbuf = upscaled;
    }
  }

  return pixbuf;
}

static void write_file(GdkPixbuf *pixbuf, char *path) {
  GError *error = NULL;
  // Use maximum PNG compression (9) - lossless, just smaller file size
  char *keys[] = {"compression", NULL};
  char *values[] = {"9", NULL};
  gdk_pixbuf_savev(pixbuf, path, "png", keys, values, &error);

  if (error != NULL) {
    g_critical("unable to save drawing area to pixbuf: %s", error->message);
    g_error_free(error);
  }
}

void pixbuf_save_state_to_folder(GdkPixbuf *pixbuf, char *folder,
                                 char *filename_format) {
  time_t current_time = time(NULL);
  char *c_time_string;
  char filename[255];
  char path[MAX_PATH];
  size_t bytes_formated;

  c_time_string = ctime(&current_time);
  c_time_string[strlen(c_time_string) - 1] = '\0';
  bytes_formated = strftime(filename, sizeof(filename), filename_format,
                            localtime(&current_time));
  if (!bytes_formated) {
    g_warning(
        "filename_format: %s overflows filename limit - file cannot be saved",
        filename_format);
    return;
  }

  g_snprintf(path, MAX_PATH, "%s/%s", folder, filename);
  g_info("saving surface to path: %s", path);
  write_file(pixbuf, path);
}

void pixbuf_save_to_stdout(GdkPixbuf *pixbuf) {
  GOutputStream *out;
  GError *error = NULL;

  out = g_unix_output_stream_new(STDOUT_FILENO, TRUE);

  gdk_pixbuf_save_to_stream(pixbuf, out, "png", NULL, &error, NULL);

  if (error != NULL) {
    g_warning("unable to save surface to stdout: %s", error->message);
    g_error_free(error);
    return;
  }

  g_object_unref(out);
}

GdkPixbuf *pixbuf_init_from_file(struct swappy_state *state) {
  GError *error = NULL;
  char *file =
      state->temp_file_str != NULL ? state->temp_file_str : state->file_str;
  GdkPixbuf *image = gdk_pixbuf_new_from_file(file, &error);

  if (error != NULL) {
    g_printerr("unable to load file: %s - reason: %s\n", file, error->message);
    g_error_free(error);
    return NULL;
  }

  state->original_image = image;
  return image;
}

void pixbuf_save_to_file(GdkPixbuf *pixbuf, char *file) {
  if (g_strcmp0(file, "-") == 0) {
    pixbuf_save_to_stdout(pixbuf);
  } else {
    write_file(pixbuf, file);
  }
}

void pixbuf_scale_surface_from_widget(struct swappy_state *state,
                                      GtkWidget *widget) {
  GtkAllocation *alloc = g_new(GtkAllocation, 1);
  GdkPixbuf *image = state->original_image;
  gtk_widget_get_allocation(widget, alloc);

  cairo_format_t format = CAIRO_FORMAT_ARGB32;
  gint image_width = gdk_pixbuf_get_width(image);
  gint image_height = gdk_pixbuf_get_height(image);

  cairo_surface_t *original_image_surface =
      cairo_image_surface_create(format, image_width, image_height);

  if (!original_image_surface) {
    g_error("unable to create cairo original surface from pixbuf");
    goto finish;
  } else {
    cairo_t *cr;
    cr = cairo_create(original_image_surface);
    gdk_cairo_set_source_pixbuf(cr, image, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
  }

  cairo_surface_t *rendering_surface =
      cairo_image_surface_create(format, image_width, image_height);

  if (!rendering_surface) {
    g_error("unable to create rendering surface");
    goto finish;
  }

  g_info("size of area to render: %ux%u", alloc->width, alloc->height);

finish:
  if (state->original_image_surface) {
    cairo_surface_destroy(state->original_image_surface);
    state->original_image_surface = NULL;
  }
  state->original_image_surface = original_image_surface;

  if (state->rendering_surface) {
    cairo_surface_destroy(state->rendering_surface);
    state->rendering_surface = NULL;
  }
  state->rendering_surface = rendering_surface;

  g_free(alloc);
}

void pixbuf_free(struct swappy_state *state) {
  if (G_IS_OBJECT(state->original_image)) {
    g_object_unref(state->original_image);
  }
}
