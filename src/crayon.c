/*
 * crayon-drawing.c
 * 
 * Cairo drawing code for Mac-style crayon color picker.
 * Each crayon has: pointed tip, colored body, paper wrapper band, highlight.
 */

#include <gtk/gtk.h>
#include <cairo.h>
#include <math.h>

/* ============================================
   Crayon Color Definitions
   ============================================ */

typedef struct {
    const char *name;
    double r, g, b;  /* 0.0 - 1.0 */
} CrayonColor;

/* Classic Mac crayon colors */
static const CrayonColor CRAYONS[18] = {
    /* Row 0: Warm colors */
    {"Cayenne",    0.580, 0.067, 0.000},
    {"Maraschino", 1.000, 0.149, 0.000},
    {"Tangerine",  1.000, 0.576, 0.000},
    {"Lemon",      1.000, 0.984, 0.000},
    {"Lime",       0.557, 0.980, 0.000},
    {"Spring",     0.000, 0.976, 0.000},
    
    /* Row 1: Cool colors */
    {"Turquoise",  0.000, 0.992, 1.000},
    {"Aqua",       0.000, 0.588, 1.000},
    {"Blueberry",  0.016, 0.200, 1.000},
    {"Grape",      0.580, 0.216, 1.000},
    {"Magenta",    1.000, 0.251, 1.000},
    {"Strawberry", 1.000, 0.184, 0.573},
    
    /* Row 2: Neutrals */
    {"Licorice",   0.000, 0.000, 0.000},
    {"Iron",       0.251, 0.251, 0.251},
    {"Nickel",     0.502, 0.502, 0.502},
    {"Aluminum",   0.749, 0.749, 0.749},
    {"Snow",       1.000, 1.000, 1.000},
    {"Mocha",      0.604, 0.322, 0.000},
};

#define CRAYON_COUNT 18
#define CRAYON_COLS 6
#define CRAYON_ROWS 3

/* Crayon dimensions */
#define CRAYON_WIDTH 28
#define CRAYON_HEIGHT 56
#define CRAYON_SPACING 4
#define TIP_HEIGHT_RATIO 0.22      /* Tip is 22% of total height */
#define WRAPPER_START_RATIO 0.35   /* Wrapper starts at 35% from top */
#define WRAPPER_HEIGHT_RATIO 0.20  /* Wrapper is 20% of total height */

/* ============================================
   Draw a Single Crayon
   ============================================ */

static void draw_crayon(cairo_t *cr, 
                        double x, double y, 
                        double width, double height,
                        const CrayonColor *color,
                        gboolean selected,
                        gboolean hover) {
    
    double tip_height = height * TIP_HEIGHT_RATIO;
    double body_top = y + tip_height;
    double body_height = height - tip_height;
    
    /* If selected, lift the crayon up */
    double lift = 0;
    if (selected) {
        lift = 6;
    } else if (hover) {
        lift = 3;
    }
    y -= lift;
    body_top -= lift;
    
    /* Calculate luminance for adaptive effects */
    double luminance = 0.299 * color->r + 0.587 * color->g + 0.114 * color->b;
    
    /* --- Drop shadow for selected crayon --- */
    if (selected) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.4);
        
        /* Shadow under the crayon body */
        cairo_rectangle(cr, x + 2, body_top + lift + 4, width, body_height);
        cairo_fill(cr);
        cairo_restore(cr);
    }
    
    /* --- Crayon Tip (pointed triangle) --- */
    cairo_save(cr);
    
    /* Tip is slightly darker than body */
    cairo_set_source_rgb(cr, 
        color->r * 0.7, 
        color->g * 0.7, 
        color->b * 0.7);
    
    /* Draw pointed tip shape */
    cairo_move_to(cr, x + width / 2, y);              /* Top point */
    cairo_line_to(cr, x + width, y + tip_height);      /* Right corner */
    cairo_line_to(cr, x, y + tip_height);              /* Left corner */
    cairo_close_path(cr);
    cairo_fill(cr);
    
    /* Add a tiny highlight on the tip */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
    cairo_move_to(cr, x + width / 2, y + 2);
    cairo_line_to(cr, x + width * 0.35, y + tip_height - 2);
    cairo_line_to(cr, x + width / 2 - 2, y + tip_height - 2);
    cairo_close_path(cr);
    cairo_fill(cr);
    
    cairo_restore(cr);
    
    /* --- Crayon Body --- */
    cairo_save(cr);
    
    /* Main body color */
    cairo_set_source_rgb(cr, color->r, color->g, color->b);
    cairo_rectangle(cr, x, body_top, width, body_height);
    cairo_fill(cr);
    
    /* Left edge highlight */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
    cairo_rectangle(cr, x + 2, body_top + 2, 3, body_height - 4);
    cairo_fill(cr);
    
    /* Right edge shadow */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.15);
    cairo_rectangle(cr, x + width - 4, body_top + 2, 3, body_height - 4);
    cairo_fill(cr);
    
    cairo_restore(cr);
    
    /* --- Paper Wrapper Band --- */
    cairo_save(cr);
    
    double wrapper_y = y + height * WRAPPER_START_RATIO;
    double wrapper_h = height * WRAPPER_HEIGHT_RATIO;
    
    /* Wrapper base (off-white/cream) */
    cairo_set_source_rgb(cr, 0.95, 0.93, 0.88);
    cairo_rectangle(cr, x, wrapper_y, width, wrapper_h);
    cairo_fill(cr);
    
    /* Wrapper top edge (slight shadow) */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.1);
    cairo_rectangle(cr, x, wrapper_y, width, 2);
    cairo_fill(cr);
    
    /* Wrapper bottom edge (highlight) */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.3);
    cairo_rectangle(cr, x, wrapper_y + wrapper_h - 2, width, 2);
    cairo_fill(cr);
    
    /* Wrapper stripe (colored line matching crayon) */
    cairo_set_source_rgba(cr, color->r, color->g, color->b, 0.6);
    cairo_rectangle(cr, x + 4, wrapper_y + wrapper_h / 2 - 1, width - 8, 2);
    cairo_fill(cr);
    
    cairo_restore(cr);
    
    /* --- Selection Ring --- */
    if (selected) {
        cairo_save(cr);
        
        /* Use contrasting color based on luminance */
        if (luminance > 0.5) {
            cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        } else {
            cairo_set_source_rgb(cr, 1, 1, 1);
        }
        
        cairo_set_line_width(cr, 2.5);
        
        /* Draw rounded rect around entire crayon */
        double margin = 3;
        double rx = x - margin;
        double ry = y - margin;
        double rw = width + margin * 2;
        double rh = height + margin * 2;
        double radius = 4;
        
        cairo_new_sub_path(cr);
        cairo_arc(cr, rx + rw - radius, ry + radius, radius, -M_PI/2, 0);
        cairo_arc(cr, rx + rw - radius, ry + rh - radius, radius, 0, M_PI/2);
        cairo_arc(cr, rx + radius, ry + rh - radius, radius, M_PI/2, M_PI);
        cairo_arc(cr, rx + radius, ry + radius, radius, M_PI, 3*M_PI/2);
        cairo_close_path(cr);
        cairo_stroke(cr);
        
        cairo_restore(cr);
    }
    
    /* --- Border for very light colors (Snow, Lemon) --- */
    if (luminance > 0.9) {
        cairo_save(cr);
        cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, 0.5);
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, x + 0.5, body_top + 0.5, width - 1, body_height - 1);
        cairo_stroke(cr);
        cairo_restore(cr);
    }
}

/* ============================================
   Draw the Wooden Tray Background
   ============================================ */

static void draw_wooden_tray(cairo_t *cr,
                             double x, double y,
                             double width, double height) {
    
    /* Outer frame gradient (lighter wood) */
    cairo_pattern_t *frame_grad = cairo_pattern_create_linear(x, y, x, y + height);
    cairo_pattern_add_color_stop_rgb(frame_grad, 0.0, 0.627, 0.533, 0.408);  /* #a08868 */
    cairo_pattern_add_color_stop_rgb(frame_grad, 1.0, 0.502, 0.408, 0.282);  /* #806848 */
    
    /* Draw outer frame with rounded corners */
    double radius = 10;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -M_PI/2, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, M_PI/2);
    cairo_arc(cr, x + radius, y + height - radius, radius, M_PI/2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3*M_PI/2);
    cairo_close_path(cr);
    
    cairo_set_source(cr, frame_grad);
    cairo_fill(cr);
    cairo_pattern_destroy(frame_grad);
    
    /* Top highlight on frame */
    cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
    cairo_rectangle(cr, x + 4, y + 2, width - 8, 2);
    cairo_fill(cr);
    
    /* Inner tray (darker, recessed) */
    double inset = 6;
    double inner_x = x + inset;
    double inner_y = y + inset;
    double inner_w = width - inset * 2;
    double inner_h = height - inset * 2;
    
    cairo_pattern_t *tray_grad = cairo_pattern_create_linear(
        inner_x, inner_y, inner_x, inner_y + inner_h);
    cairo_pattern_add_color_stop_rgb(tray_grad, 0.0, 0.439, 0.345, 0.220);  /* #705838 */
    cairo_pattern_add_color_stop_rgb(tray_grad, 1.0, 0.290, 0.220, 0.125);  /* #4a3820 */
    
    double inner_radius = 6;
    cairo_new_sub_path(cr);
    cairo_arc(cr, inner_x + inner_w - inner_radius, inner_y + inner_radius, 
              inner_radius, -M_PI/2, 0);
    cairo_arc(cr, inner_x + inner_w - inner_radius, inner_y + inner_h - inner_radius, 
              inner_radius, 0, M_PI/2);
    cairo_arc(cr, inner_x + inner_radius, inner_y + inner_h - inner_radius, 
              inner_radius, M_PI/2, M_PI);
    cairo_arc(cr, inner_x + inner_radius, inner_y + inner_radius, 
              inner_radius, M_PI, 3*M_PI/2);
    cairo_close_path(cr);
    
    cairo_set_source(cr, tray_grad);
    cairo_fill(cr);
    cairo_pattern_destroy(tray_grad);
    
    /* Inner shadow (recessed effect) */
    cairo_set_source_rgba(cr, 0, 0, 0, 0.3);
    cairo_rectangle(cr, inner_x, inner_y, inner_w, 3);
    cairo_fill(cr);
    
    cairo_set_source_rgba(cr, 0, 0, 0, 0.2);
    cairo_rectangle(cr, inner_x, inner_y, 2, inner_h);
    cairo_fill(cr);
}

/* ============================================
   Draw Complete Crayon Grid
   ============================================ */

typedef struct {
    int selected_index;    /* Currently selected crayon (0-17, or -1) */
    int hover_index;       /* Currently hovered crayon (0-17, or -1) */
    gboolean editing_fill; /* TRUE = editing fill, FALSE = editing stroke */
} CrayonBoxState;

static gboolean draw_crayon_box(GtkWidget *widget, 
                                 cairo_t *cr, 
                                 gpointer user_data) {
    CrayonBoxState *state = (CrayonBoxState *)user_data;
    
    int widget_width = gtk_widget_get_allocated_width(widget);
    int widget_height = gtk_widget_get_allocated_height(widget);
    
    /* Calculate grid dimensions */
    double grid_width = CRAYON_COLS * CRAYON_WIDTH + (CRAYON_COLS - 1) * CRAYON_SPACING;
    double grid_height = CRAYON_ROWS * CRAYON_HEIGHT + (CRAYON_ROWS - 1) * CRAYON_SPACING;
    
    /* Tray padding */
    double tray_padding = 16;
    double tray_width = grid_width + tray_padding * 2;
    double tray_height = grid_height + tray_padding * 2 + 10; /* Extra for lift room */
    
    /* Center the tray */
    double tray_x = (widget_width - tray_width) / 2;
    double tray_y = (widget_height - tray_height) / 2;
    
    /* Draw wooden tray background */
    draw_wooden_tray(cr, tray_x, tray_y, tray_width, tray_height);
    
    /* Draw crayons */
    double start_x = tray_x + tray_padding;
    double start_y = tray_y + tray_padding + 8; /* Offset for lift room */
    
    for (int i = 0; i < CRAYON_COUNT; i++) {
        int row = i / CRAYON_COLS;
        int col = i % CRAYON_COLS;
        
        double cx = start_x + col * (CRAYON_WIDTH + CRAYON_SPACING);
        double cy = start_y + row * (CRAYON_HEIGHT + CRAYON_SPACING);
        
        gboolean selected = (i == state->selected_index);
        gboolean hover = (i == state->hover_index) && !selected;
        
        draw_crayon(cr, cx, cy, CRAYON_WIDTH, CRAYON_HEIGHT, 
                    &CRAYONS[i], selected, hover);
    }
    
    return FALSE;
}

/* ============================================
   Hit Testing (which crayon was clicked)
   ============================================ */

static int crayon_hit_test(GtkWidget *widget, double mouse_x, double mouse_y) {
    int widget_width = gtk_widget_get_allocated_width(widget);
    int widget_height = gtk_widget_get_allocated_height(widget);
    
    double grid_width = CRAYON_COLS * CRAYON_WIDTH + (CRAYON_COLS - 1) * CRAYON_SPACING;
    double grid_height = CRAYON_ROWS * CRAYON_HEIGHT + (CRAYON_ROWS - 1) * CRAYON_SPACING;
    
    double tray_padding = 16;
    double tray_width = grid_width + tray_padding * 2;
    double tray_height = grid_height + tray_padding * 2 + 10;
    
    double tray_x = (widget_width - tray_width) / 2;
    double tray_y = (widget_height - tray_height) / 2;
    
    double start_x = tray_x + tray_padding;
    double start_y = tray_y + tray_padding + 8;
    
    /* Check each crayon */
    for (int i = 0; i < CRAYON_COUNT; i++) {
        int row = i / CRAYON_COLS;
        int col = i % CRAYON_COLS;
        
        double cx = start_x + col * (CRAYON_WIDTH + CRAYON_SPACING);
        double cy = start_y + row * (CRAYON_HEIGHT + CRAYON_SPACING);
        
        /* Expand hit area slightly for easier clicking */
        double margin = 2;
        if (mouse_x >= cx - margin && mouse_x <= cx + CRAYON_WIDTH + margin &&
            mouse_y >= cy - margin && mouse_y <= cy + CRAYON_HEIGHT + margin) {
            return i;
        }
    }
    
    return -1; /* No hit */
}

/* ============================================
   Event Handlers
   ============================================ */

static gboolean on_crayon_click(GtkWidget *widget,
                                 GdkEventButton *event,
                                 gpointer user_data) {
    CrayonBoxState *state = (CrayonBoxState *)user_data;

    if (event->button == 1) { /* Left click */
        int hit = crayon_hit_test(widget, event->x, event->y);
        if (hit >= 0) {
            state->selected_index = hit;
            gtk_widget_queue_draw(widget);

            /* Call the callback if set */
            if (state->callback && state->callback_data) {
                state->callback(state->callback_data,
                               CRAYONS[hit].r,
                               CRAYONS[hit].g,
                               CRAYONS[hit].b,
                               CRAYONS[hit].name);
            }
        }
    }

    return TRUE;
}

static gboolean on_crayon_motion(GtkWidget *widget,
                                  GdkEventMotion *event,
                                  gpointer user_data) {
    CrayonBoxState *state = (CrayonBoxState *)user_data;
    
    int hit = crayon_hit_test(widget, event->x, event->y);
    if (hit != state->hover_index) {
        state->hover_index = hit;
        gtk_widget_queue_draw(widget);
    }
    
    return TRUE;
}

static gboolean on_crayon_leave(GtkWidget *widget,
                                 GdkEventCrossing *event,
                                 gpointer user_data) {
    CrayonBoxState *state = (CrayonBoxState *)user_data;
    state->hover_index = -1;
    gtk_widget_queue_draw(widget);
    return TRUE;
}

/* ============================================
   Create Crayon Box Widget
   ============================================ */

GtkWidget* create_crayon_box_widget(CrayonBoxState *state) {
    /* Initialize state */
    state->selected_index = 1;  /* Default to Maraschino */
    state->hover_index = -1;
    state->editing_fill = FALSE;
    
    /* Calculate required size */
    double grid_width = CRAYON_COLS * CRAYON_WIDTH + (CRAYON_COLS - 1) * CRAYON_SPACING;
    double grid_height = CRAYON_ROWS * CRAYON_HEIGHT + (CRAYON_ROWS - 1) * CRAYON_SPACING;
    double tray_padding = 16;
    int width = (int)(grid_width + tray_padding * 2);
    int height = (int)(grid_height + tray_padding * 2 + 16); /* Extra for lift */
    
    /* Create drawing area */
    GtkWidget *drawing_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area, width, height);
    
    /* Enable events */
    gtk_widget_add_events(drawing_area, 
        GDK_BUTTON_PRESS_MASK | 
        GDK_POINTER_MOTION_MASK |
        GDK_LEAVE_NOTIFY_MASK);
    
    /* Connect signals */
    g_signal_connect(drawing_area, "draw", 
                     G_CALLBACK(draw_crayon_box), state);
    g_signal_connect(drawing_area, "button-press-event", 
                     G_CALLBACK(on_crayon_click), state);
    g_signal_connect(drawing_area, "motion-notify-event", 
                     G_CALLBACK(on_crayon_motion), state);
    g_signal_connect(drawing_area, "leave-notify-event", 
                     G_CALLBACK(on_crayon_leave), state);
    
    return drawing_area;
}

/* ============================================
   Getters for Current Color
   ============================================ */

void crayon_get_selected_rgb(CrayonBoxState *state, 
                              double *r, double *g, double *b) {
    if (state->selected_index >= 0 && state->selected_index < CRAYON_COUNT) {
        *r = CRAYONS[state->selected_index].r;
        *g = CRAYONS[state->selected_index].g;
        *b = CRAYONS[state->selected_index].b;
    }
}

const char* crayon_get_selected_name(CrayonBoxState *state) {
    if (state->selected_index >= 0 && state->selected_index < CRAYON_COUNT) {
        return CRAYONS[state->selected_index].name;
    }
    return NULL;
}

/* ============================================
   Example Usage / Test
   ============================================ */

#ifdef CRAYON_TEST_MAIN

static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Crayon Box Test");
    gtk_window_set_default_size(GTK_WINDOW(window), 300, 280);
    
    /* Dark background */
    GdkRGBA bg = {0.1, 0.1, 0.1, 1.0};
    gtk_widget_override_background_color(window, GTK_STATE_FLAG_NORMAL, &bg);
    
    /* Create crayon box */
    static CrayonBoxState state;
    GtkWidget *crayon_box = create_crayon_box_widget(&state);
    
    gtk_container_add(GTK_CONTAINER(window), crayon_box);
    gtk_widget_show_all(window);
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("com.example.crayontest", 
                                               G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}

#endif /* CRAYON_TEST_MAIN */
