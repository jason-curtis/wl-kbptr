#include "config.h"
#include "mode.h"
#include "state.h"
#include "utils.h"
#include "utils_cairo.h"
#include "utils_wayland.h"

#include <stdlib.h>
#include <string.h>

// Recursive keyboard-layout grid. Each level divides the current area into a
// rows x cols grid that mirrors a physical key block (e.g. the three QWERTY
// rows). The key's position on the keyboard maps to its position on screen, so
// muscle memory does the aiming. Pressing a key recurses into that cell; this
// repeats until Space/Enter clicks.

#define KBD_MAX_HISTORY 16
#define KBD_MAX_ROWS    8
#define KBD_MAX_COLS    16
#define KBD_MAX_CELLS   (KBD_MAX_ROWS * KBD_MAX_COLS)

struct kbd_mode_state {
    struct rect areas[KBD_MAX_HISTORY];
    int         current;

    // Parsed layout: cells[r * cols + c] is the key character for that cell.
    char cells[KBD_MAX_CELLS];
    int  rows;
    int  cols;

    cairo_font_face_t *label_font_face;
};

// Parse the space-separated layout string (one token per row) into the cell
// grid. Returns false if the layout is empty or malformed.
static bool parse_layout(struct kbd_mode_state *ms, const char *keys) {
    char buf[KBD_MAX_CELLS + KBD_MAX_ROWS + 1];
    strncpy(buf, keys, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    ms->rows = 0;
    ms->cols = 0;

    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok != NULL;
         tok      = strtok_r(NULL, " \t", &save)) {
        int len = strlen(tok);
        if (len == 0) {
            continue;
        }

        if (ms->rows == 0) {
            ms->cols = len;
        } else if (len != ms->cols) {
            // All rows must have the same number of keys.
            return false;
        }

        if (ms->rows >= KBD_MAX_ROWS || ms->cols > KBD_MAX_COLS) {
            return false;
        }

        for (int c = 0; c < ms->cols; c++) {
            ms->cells[ms->rows * ms->cols + c] = tok[c];
        }
        ms->rows++;
    }

    return ms->rows > 0 && ms->cols > 0;
}

// Compute the sub-rectangle for cell (r, c), distributing the remainder pixels
// across the first rows/columns so the grid covers the whole area exactly.
static struct rect
cell_rect(const struct rect *a, int rows, int cols, int r, int c) {
    int cell_w     = a->w / cols;
    int cell_w_off = a->w % cols;
    int cell_h     = a->h / rows;
    int cell_h_off = a->h % rows;

    struct rect out;
    out.x = a->x + c * cell_w + min(c, cell_w_off);
    out.y = a->y + r * cell_h + min(r, cell_h_off);
    out.w = cell_w + (c < cell_w_off ? 1 : 0);
    out.h = cell_h + (r < cell_h_off ? 1 : 0);
    return out;
}

// Expand a selected cell's width (centered, clamped to root) so that the next
// rows x cols grid has square cells. The 10:3 key block makes cells tall and
// narrow; widening the region to a cols:rows aspect ratio re-squares them and
// throttles horizontal convergence to match the vertical rate. Only ever grows
// the region, so the selected cell always stays inside it.
static struct rect
square_region(struct rect cell, struct rect root, int rows, int cols) {
    int target_w = (int)((double)cell.h * cols / rows + 0.5);
    if (target_w <= cell.w) {
        return cell; // already wide enough
    }
    if (target_w > root.w) {
        target_w = root.w;
    }

    int cx    = cell.x + cell.w / 2;
    int new_x = cx - target_w / 2;
    if (new_x < root.x) {
        new_x = root.x;
    }
    if (new_x + target_w > root.x + root.w) {
        new_x = root.x + root.w - target_w;
    }

    return (struct rect){.x = new_x, .y = cell.y, .w = target_w, .h = cell.h};
}

static void kbd_mode_move_pointer(struct state *state, struct kbd_mode_state *ms) {
    struct rect *r = &ms->areas[ms->current];
    move_pointer(state, r->x + r->w / 2, r->y + r->h / 2, CLICK_NONE);
}

void *kbd_mode_enter(struct state *state, struct rect area) {
    struct kbd_mode_state *ms = calloc(1, sizeof(*ms));
    ms->areas[0]              = area;
    ms->current              = 0;

    if (!parse_layout(ms, state->config.mode_kbd.keys)) {
        // Fall back to a sane default if the configured layout is invalid.
        parse_layout(ms, "qwertyuiop asdfghjkl; zxcvbnm,./");
    }

    ms->label_font_face = cairo_toy_font_face_create(
        state->config.mode_kbd.label_font_family, CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_NORMAL
    );

    kbd_mode_move_pointer(state, ms);

    return ms;
}

static void
kbd_mode_render(struct state *state, void *mode_state, cairo_t *cairo) {
    struct mode_kbd_config *config = &state->config.mode_kbd;
    struct kbd_mode_state  *ms     = mode_state;
    struct rect            *area   = &ms->areas[ms->current];

    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_u32(cairo, config->unselectable_bg_color);
    cairo_paint(cairo);

    // Outline previously selected areas.
    cairo_set_source_u32(cairo, config->history_border_color);
    cairo_set_line_width(cairo, 1);
    for (int i = 0; i < ms->current; i++) {
        struct rect *a = &ms->areas[i];
        cairo_rectangle(cairo, a->x + .5, a->y + .5, a->w - 1, a->h - 1);
        cairo_stroke(cairo);
    }

    if (ms->current + 1 < KBD_MAX_HISTORY) {
        struct rect cell0 = cell_rect(area, ms->rows, ms->cols, 0, 0);
        double      font_size =
            max(min(cell0.h * .6, cell0.w * .9), 1);

        cairo_set_font_face(cairo, ms->label_font_face);
        cairo_set_font_size(cairo, font_size);

        bool draw_labels = cell0.h >= 10 && cell0.w >= 6;

        for (int r = 0; r < ms->rows; r++) {
            for (int c = 0; c < ms->cols; c++) {
                struct rect cell = cell_rect(area, ms->rows, ms->cols, r, c);

                cairo_set_source_u32(
                    cairo, (r + c) % 2 == 0 ? config->even_area_bg_color
                                            : config->odd_area_bg_color
                );
                cairo_rectangle(cairo, cell.x, cell.y, cell.w, cell.h);
                cairo_fill(cairo);

                cairo_set_source_u32(
                    cairo, (r + c) % 2 == 0 ? config->even_area_border_color
                                            : config->odd_area_border_color
                );
                cairo_rectangle(
                    cairo, cell.x + .5, cell.y + .5, cell.w - 1, cell.h - 1
                );
                cairo_stroke(cairo);

                if (draw_labels) {
                    char label[2] = {ms->cells[r * ms->cols + c], '\0'};
                    cairo_text_extents_t te;
                    cairo_text_extents(cairo, label, &te);
                    cairo_set_source_u32(cairo, config->label_color);
                    cairo_move_to(
                        cairo,
                        cell.x + cell.w / 2.0 - (te.width / 2 + te.x_bearing),
                        cell.y + cell.h / 2.0 - (te.height / 2 + te.y_bearing)
                    );
                    cairo_show_text(cairo, label);
                }
            }
        }
    }

    // Pointer crosshair at the centre of the current area.
    cairo_set_line_width(cairo, 1);
    cairo_set_source_u32(cairo, config->pointer_color);
    const int px = area->x + area->w / 2;
    const int py = area->y + area->h / 2;
    cairo_move_to(cairo, px + .5, py - (int)(config->pointer_size / 2) + .5);
    cairo_line_to(cairo, px + .5, py + (int)(config->pointer_size / 2) + .5);
    cairo_stroke(cairo);
    cairo_move_to(cairo, px - (int)(config->pointer_size / 2) + .5, py + .5);
    cairo_line_to(cairo, px + (int)(config->pointer_size / 2) + .5, py + .5);
    cairo_stroke(cairo);
}

static bool kbd_mode_key(
    struct state *state, void *mode_state, xkb_keysym_t keysym, char *text
) {
    struct kbd_mode_state *ms = mode_state;

    switch (keysym) {
    case XKB_KEY_Escape:
        state->running = false;
        return true;

    case XKB_KEY_Return:
    case XKB_KEY_space:
        enter_next_mode(state, ms->areas[ms->current]);
        return true;

    case XKB_KEY_BackSpace:
        if (ms->current > 0) {
            ms->current--;
            kbd_mode_move_pointer(state, ms);
        } else {
            reenter_prev_mode(state);
        }
        return true;

    default:
        if (ms->current + 1 >= KBD_MAX_HISTORY) {
            return false;
        }

        if (text == NULL || text[0] == '\0' || text[1] != '\0') {
            return false;
        }

        for (int i = 0; i < ms->rows * ms->cols; i++) {
            if (ms->cells[i] == text[0]) {
                int         r = i / ms->cols;
                int         c = i % ms->cols;
                struct rect cell =
                    cell_rect(&ms->areas[ms->current], ms->rows, ms->cols, r, c);
                if (state->config.mode_kbd.square) {
                    cell = square_region(cell, ms->areas[0], ms->rows, ms->cols);
                }
                ms->areas[ms->current + 1] = cell;
                ms->current++;
                kbd_mode_move_pointer(state, ms);
                return true;
            }
        }

        return false;
    }
}

void kbd_mode_reenter(struct state *state, void *mode_state) {
    kbd_mode_move_pointer(state, mode_state);
}

void kbd_mode_free(void *mode_state) {
    struct kbd_mode_state *ms = mode_state;
    cairo_font_face_destroy(ms->label_font_face);
    free(ms);
}

struct mode_interface kbd_mode_interface = {
    .name    = "kbd",
    .enter   = kbd_mode_enter,
    .reenter = kbd_mode_reenter,
    .key     = kbd_mode_key,
    .render  = kbd_mode_render,
    .free    = kbd_mode_free,
};
