#include "config.h"
#include "label.h"
#include "mode.h"
#include "state.h"
#include "utils.h"
#include "utils_cairo.h"

#include <cairo.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#define MIN_SUB_AREA_SIZE (25 * 50)

void *tile_mode_enter(struct state *state, struct rect area) {
    struct tile_mode_state *ms = malloc(sizeof(*ms));
    ms->area                   = area;

    const int max_num_sub_areas = 26 * 26;

    // In all-outputs mode, base cell size on the average logical monitor area
    // rather than the full bounding box so cell density is the same as in
    // single-output mode.  Each monitor ends up with roughly the same number
    // of cells as it would have had on its own.
    int32_t density_area;
    if (state->config.general.all_outputs &&
        !wl_list_empty(&state->overlay_surfaces)) {
        int64_t total = 0;
        int     n     = 0;
        struct overlay_surface *ov;
        wl_list_for_each (ov, &state->overlay_surfaces, link) {
            if (ov->output != NULL) {
                total += (int64_t)ov->output->width * ov->output->height;
                n++;
            }
        }
        density_area =
            (n > 0) ? (int32_t)(total / n) : ms->area.w * ms->area.h;
    } else {
        density_area = ms->area.w * ms->area.h;
    }

    const int sub_area_size =
        max(density_area / max_num_sub_areas, MIN_SUB_AREA_SIZE);

    ms->sub_area_height = sqrt(sub_area_size / 2.);
    ms->sub_area_rows   = ms->area.h / ms->sub_area_height;
    if (ms->sub_area_rows == 0) {
        ms->sub_area_rows = 1;
    }
    ms->sub_area_height_off = ms->area.h % ms->sub_area_rows;
    ms->sub_area_height     = ms->area.h / ms->sub_area_rows;

    ms->sub_area_width   = sqrt(sub_area_size * 2);
    ms->sub_area_columns = ms->area.w / ms->sub_area_width;
    if (ms->sub_area_columns == 0) {
        ms->sub_area_columns = 1;
    }
    ms->sub_area_width_off = ms->area.w % ms->sub_area_columns;
    ms->sub_area_width     = ms->area.w / ms->sub_area_columns;

    ms->label_symbols =
        label_symbols_from_str(state->config.mode_tile.label_symbols);
    if (ms->label_symbols == NULL) {
        ms->label_selection = NULL;
        ms->cell_idx_map    = NULL;
        state->running      = false;
        return ms;
    }

    int total_cells  = ms->sub_area_rows * ms->sub_area_columns;
    ms->cell_idx_map = NULL;

    if (state->config.general.all_outputs &&
        !wl_list_empty(&state->overlay_surfaces)) {
        // Only assign labels to cells whose centre falls on a real output,
        // skipping dead-zone gaps between monitors.
        int *tmp       = malloc(total_cells * sizeof(int));
        int  num_valid = 0;

        for (int ci = 0; ci < total_cells; ci++) {
            int column = ci / ms->sub_area_rows;
            int row    = ci % ms->sub_area_rows;

            int x = column * ms->sub_area_width +
                    min(column, ms->sub_area_width_off);
            int w = ms->sub_area_width +
                    (column < ms->sub_area_width_off ? 1 : 0);
            int y =
                row * ms->sub_area_height + min(row, ms->sub_area_height_off);
            int h = ms->sub_area_height +
                    (row < ms->sub_area_height_off ? 1 : 0);

            int32_t cx = ms->area.x + x + w / 2;
            int32_t cy = ms->area.y + y + h / 2;

            bool           on_output = false;
            struct output *out;
            wl_list_for_each (out, &state->outputs, link) {
                if (cx >= out->x && cx < out->x + out->width &&
                    cy >= out->y && cy < out->y + out->height) {
                    on_output = true;
                    break;
                }
            }
            if (on_output) {
                tmp[num_valid++] = ci;
            }
        }

        if (num_valid < total_cells) {
            ms->cell_idx_map = malloc(num_valid * sizeof(int));
            memcpy(ms->cell_idx_map, tmp, num_valid * sizeof(int));
            ms->label_selection =
                label_selection_new(ms->label_symbols, num_valid);
        } else {
            ms->label_selection =
                label_selection_new(ms->label_symbols, total_cells);
        }
        free(tmp);
    } else {
        ms->label_selection =
            label_selection_new(ms->label_symbols, total_cells);
    }

    ms->label_font_face = cairo_toy_font_face_create(
        state->config.mode_tile.label_font_family, CAIRO_FONT_SLANT_NORMAL,
        CAIRO_FONT_WEIGHT_NORMAL
    );

    return ms;
}

// `tile_mode_back` goes back in history. Returns true if there was something to
// go back to.
static bool tile_mode_back(struct tile_mode_state *mode_state) {
    return label_selection_back(mode_state->label_selection) != 0;
}

// `tile_mode_reenter` reenters the tile mode. We assume that the saved state is
// valid and goes back in history once.
void tile_mode_reenter(struct state *state, void *mode_state) {
    struct tile_mode_state *ms = mode_state;
    tile_mode_back(ms);
}

static struct rect
idx_to_rect(struct tile_mode_state *mode_state, int idx, int x_off, int y_off) {
    int column = idx / mode_state->sub_area_rows;
    int row    = idx % mode_state->sub_area_rows;

    return (struct rect){
        .x = column * mode_state->sub_area_width +
             min(column, mode_state->sub_area_width_off) + x_off,
        .w = mode_state->sub_area_width +
             (column < mode_state->sub_area_width_off ? 1 : 0),
        .y = row * mode_state->sub_area_height +
             min(row, mode_state->sub_area_height_off) + y_off,
        .h = mode_state->sub_area_height +
             (row < mode_state->sub_area_height_off ? 1 : 0),
    };
}

static bool tile_mode_key(
    struct state *state, void *mode_state, xkb_keysym_t keysym, char *text
) {
    struct tile_mode_state *ms = mode_state;

    switch (keysym) {
    case XKB_KEY_BackSpace:
        return tile_mode_back(ms);
        break;

    case XKB_KEY_Escape:
        state->running = false;
        break;
    default:;
        int symbol_idx = label_symbols_find_idx(ms->label_symbols, text);
        if (symbol_idx < 0) {
            return false;
        }

        label_selection_append(ms->label_selection, symbol_idx);

        int label_idx = label_selection_to_idx(ms->label_selection);
        if (label_idx >= 0) {
            int cell_idx =
                ms->cell_idx_map ? ms->cell_idx_map[label_idx] : label_idx;
            enter_next_mode(
                state, idx_to_rect(ms, cell_idx, ms->area.x, ms->area.y)
            );
        }
        return true;
    }

    return false;
}

void tile_mode_render(struct state *state, void *mode_state, cairo_t *cairo) {
    struct mode_tile_config *config = &state->config.mode_tile;
    struct tile_mode_state  *ms     = mode_state;

    cairo_set_font_face(cairo, ms->label_font_face);
    cairo_set_font_size(
        cairo, compute_relative_font_size(
                   &config->label_font_size, ms->sub_area_height
               )
    );

    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_u32(cairo, config->unselectable_bg_color);
    cairo_paint(cairo);

    cairo_translate(cairo, ms->area.x, ms->area.y);

    cairo_set_source_u32(cairo, config->unselectable_bg_color);
    cairo_rectangle(cairo, .5, .5, ms->area.w - 1, ms->area.h - 1);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);

    int num_labels = ms->label_selection->num_labels;
    label_selection_t *curr_label =
        label_selection_new(ms->label_symbols, num_labels);
    label_selection_set_from_idx(curr_label, 0);

    int  label_str_max_len = label_selection_str_max_len(curr_label) + 1;
    char label_selected_str[label_str_max_len];
    char label_unselected_str[label_str_max_len];

    for (int li = 0; li < num_labels; li++) {
        int ci     = ms->cell_idx_map ? ms->cell_idx_map[li] : li;
        int column = ci / ms->sub_area_rows;
        int row    = ci % ms->sub_area_rows;

        const int x =
            column * ms->sub_area_width + min(column, ms->sub_area_width_off);
        const int w =
            ms->sub_area_width + (column < ms->sub_area_width_off ? 1 : 0);
        const int y =
            row * ms->sub_area_height + min(row, ms->sub_area_height_off);
        const int h =
            ms->sub_area_height + (row < ms->sub_area_height_off ? 1 : 0);

        const bool selectable =
            label_selection_is_included(curr_label, ms->label_selection);

        cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
        if (selectable) {
            cairo_set_source_u32(cairo, config->selectable_bg_color);
            cairo_rectangle(cairo, x, y, w, h);
            cairo_fill(cairo);

            cairo_set_source_u32(cairo, config->selectable_border_color);
            cairo_rectangle(cairo, x + .5, y + .5, w - 1, h - 1);
            cairo_set_line_width(cairo, 1);
            cairo_stroke(cairo);

            cairo_text_extents_t te_all;
            label_selection_str(curr_label, label_selected_str);
            cairo_text_extents(cairo, label_selected_str, &te_all);

            label_selection_str_split(
                curr_label, label_selected_str, label_unselected_str,
                ms->label_selection->next
            );

            cairo_text_extents_t te_selected, te_unselected;
            cairo_text_extents(cairo, label_selected_str, &te_selected);
            cairo_text_extents(cairo, label_unselected_str, &te_unselected);

            // Centers the label.
            cairo_move_to(
                cairo,
                x + (w - te_selected.x_advance - te_unselected.x_advance) / 2,
                y + (int)((h + te_all.height) / 2)
            );
            cairo_set_source_u32(cairo, config->label_select_color);
            cairo_show_text(cairo, label_selected_str);
            cairo_set_source_u32(cairo, config->label_color);
            cairo_show_text(cairo, label_unselected_str);
        }

        label_selection_incr(curr_label);
    }

    label_selection_free(curr_label);
    cairo_translate(cairo, -ms->area.x, -ms->area.y);
}

void tile_mode_state_free(void *mode_state) {
    struct tile_mode_state *ms = mode_state;
    cairo_font_face_destroy(ms->label_font_face);
    label_selection_free(ms->label_selection);
    label_symbols_free(ms->label_symbols);
    free(ms->cell_idx_map);
    free(ms);
}

struct mode_interface tile_mode_interface = {
    .name    = "tile",
    .enter   = tile_mode_enter,
    .reenter = tile_mode_reenter,
    .key     = tile_mode_key,
    .render  = tile_mode_render,
    .free    = tile_mode_state_free,
};
