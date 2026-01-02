/*
 * crayon-drawing.h
 *
 * Mac-style crayon color picker for GTK3
 */

#ifndef CRAYON_DRAWING_H
#define CRAYON_DRAWING_H

#include <gtk/gtk.h>

/* Forward declaration for callback */
struct swappy_state;

/* Callback function type for color selection */
typedef void (*CrayonColorCallback)(struct swappy_state *state,
                                     double r, double g, double b,
                                     const char *name);

/* Crayon box state */
typedef struct {
    int selected_index;    /* Currently selected crayon (0-17, or -1) */
    int hover_index;       /* Currently hovered crayon (0-17, or -1) */
    gboolean editing_fill; /* TRUE = editing fill, FALSE = editing stroke */
    CrayonColorCallback callback;
    struct swappy_state *callback_data;
} CrayonBoxState;

/* Create a new crayon box drawing area widget */
GtkWidget* create_crayon_box_widget(CrayonBoxState *state);

/* Initialize an existing GtkDrawingArea as a crayon box */
void crayon_box_init(GtkDrawingArea *drawing_area, CrayonBoxState *state);

/* Set the color change callback */
void crayon_box_set_callback(CrayonBoxState *state,
                              CrayonColorCallback callback,
                              struct swappy_state *data);

/* Get the selected color as RGB (0.0 - 1.0) */
void crayon_get_selected_rgb(CrayonBoxState *state, 
                              double *r, double *g, double *b);

/* Get the selected color name (e.g., "Maraschino") */
const char* crayon_get_selected_name(CrayonBoxState *state);

/* Color constants for reference */
#define CRAYON_COUNT 18

/* Color indices */
enum {
    CRAYON_CAYENNE = 0,
    CRAYON_MARASCHINO,
    CRAYON_TANGERINE,
    CRAYON_LEMON,
    CRAYON_LIME,
    CRAYON_SPRING,
    CRAYON_TURQUOISE,
    CRAYON_AQUA,
    CRAYON_BLUEBERRY,
    CRAYON_GRAPE,
    CRAYON_MAGENTA,
    CRAYON_STRAWBERRY,
    CRAYON_LICORICE,
    CRAYON_IRON,
    CRAYON_NICKEL,
    CRAYON_ALUMINUM,
    CRAYON_SNOW,
    CRAYON_MOCHA
};

#endif /* CRAYON_DRAWING_H */
