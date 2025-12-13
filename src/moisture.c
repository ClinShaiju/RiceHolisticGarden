#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------- Config ---------------- */

#define MAX_PLOTS 20
#define VISIBLE_SLOTS 4
#define ANIM_TIME 300

/* Screen & Layout Constants for 1280x800 */
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 800

#define SLOT_WIDTH 220
#define SLOT_HEIGHT 600
#define SLOT_GAP 40
#define SLOT_SPACING (SLOT_WIDTH + SLOT_GAP)

/* Calculate centering offset for the group of slots */
#define TOTAL_GROUP_WIDTH ((VISIBLE_SLOTS * SLOT_WIDTH) + ((VISIBLE_SLOTS - 1) * SLOT_GAP))
#define START_X ((SCREEN_WIDTH - TOTAL_GROUP_WIDTH) / 2)
#define START_Y ((SCREEN_HEIGHT - SLOT_HEIGHT) / 2)

/* Internal UI Elements */
#define TRACK_WIDTH 60
#define TRACK_HEIGHT 450
#define TRACK_Y_OFFSET 70
#define BUTTON_SIZE 80
#define BUTTON_MARGIN 20

/* Persistence Config */
#define SAVE_FILE_PATH "plots.bin"
#define SAVE_MAGIC 0x4D445031 /* 'MDP1' */

/* ---------------- Data Model ---------------- */

typedef struct {
    char name[32];
    int32_t sensor_id;
    int32_t threshold;
    int32_t moisture;
    int32_t sim_target;
    int32_t sim_step;
} plot_data_t;

/* File Header Structure for Save/Load */
typedef struct {
    int32_t magic;
    int32_t version;
    int32_t plot_count;
    int32_t plot_name_counter;
    plot_data_t plots[MAX_PLOTS];
} plots_file_t;

static plot_data_t all_plots[MAX_PLOTS];
static int plot_count = 0;
static int plot_name_counter = 0; /* Persistent counter for naming */
static int scroll_offset = 0;

/* ---------------- Visual Handles ---------------- */

typedef struct {
    lv_obj_t* container;
    lv_obj_t* cover;
    lv_obj_t* top_line;
    lv_obj_t* label_percent;
    lv_obj_t* label_name;
    lv_obj_t* slider;
} slot_handles_t;

static slot_handles_t slots_storage[VISIBLE_SLOTS + 1];
static slot_handles_t* ui_slots[VISIBLE_SLOTS];
static slot_handles_t* spare_slot;

static lv_obj_t* btn_edit;
static lv_obj_t* btn_edit_label;
static lv_obj_t* btn_add;
static lv_obj_t* btn_del;
static lv_obj_t* btn_rename;
static lv_obj_t* btn_left;
static lv_obj_t* btn_right;

/* Modes */
static bool edit_mode = false;
static bool delete_mode = false;
static bool rename_mode = false;
static bool is_animating = false;

/* Interaction State */
static int pending_delete_idx = -1;
static int pending_rename_idx = -1;
static lv_obj_t* rename_mbox = NULL;
static lv_obj_t* rename_ta = NULL;
static lv_obj_t* rename_kb = NULL;

/* ---------------- Gradients ---------------- */

static lv_grad_dsc_t g_top;
static lv_grad_dsc_t g_btm;
static bool grads_initialized = false;

static void init_gradients(void) {
    if (grads_initialized) return;

    g_top.dir = LV_GRAD_DIR_VER;
    g_top.stops_count = 2;
    g_top.stops[0].color = lv_color_hex(0x00FF55);
    g_top.stops[0].opa = LV_OPA_COVER;
    g_top.stops[0].frac = 0;
    g_top.stops[1].color = lv_color_hex(0xFFFF00);
    g_top.stops[1].opa = LV_OPA_COVER;
    g_top.stops[1].frac = 255;

    g_btm.dir = LV_GRAD_DIR_VER;
    g_btm.stops_count = 2;
    g_btm.stops[0].color = lv_color_hex(0xFFFF00);
    g_btm.stops[0].opa = LV_OPA_COVER;
    g_btm.stops[0].frac = 0;
    g_btm.stops[1].color = lv_color_hex(0xFF0000);
    g_btm.stops[1].opa = LV_OPA_COVER;
    g_btm.stops[1].frac = 255;

    grads_initialized = true;
}

/* ---------------- Persistence ---------------- */

static void save_plots_to_disk(void) {
    FILE* f = fopen(SAVE_FILE_PATH, "wb");
    if (!f) return;

    plots_file_t disk;
    memset(&disk, 0, sizeof(disk));
    disk.magic = SAVE_MAGIC;
    disk.version = 1;
    disk.plot_count = plot_count;
    disk.plot_name_counter = plot_name_counter;

    for (int i = 0; i < MAX_PLOTS; i++) {
        if (i < plot_count) disk.plots[i] = all_plots[i];
    }

    fwrite(&disk, sizeof(disk), 1, f);
    fclose(f);
}

static bool load_plots_from_disk(void) {
    FILE* f = fopen(SAVE_FILE_PATH, "rb");
    if (!f) return false;

    plots_file_t disk;
    size_t n = fread(&disk, sizeof(disk), 1, f);
    fclose(f);

    if (n != 1) return false;
    if (disk.magic != SAVE_MAGIC || disk.version != 1) return false;
    if (disk.plot_count < 0 || disk.plot_count > MAX_PLOTS) return false;

    plot_count = disk.plot_count;
    plot_name_counter = disk.plot_name_counter;

    for (int i = 0; i < plot_count; i++) {
        all_plots[i] = disk.plots[i];
    }

    return true;
}

/* ---------------- Helpers ---------------- */

static void add_new_plot(void) {
    if (plot_count >= MAX_PLOTS) return;

    int id = plot_count;
    plot_data_t* p = &all_plots[id];

    plot_name_counter++;
    snprintf(p->name, sizeof(p->name), "Plot #%d", plot_name_counter);
    p->sensor_id = id + 100;
    p->threshold = 50;
    p->moisture = 50;
    p->sim_target = 50;
    p->sim_step = 0;

    plot_count++;
}

static lv_coord_t get_slot_x(int index) {
    return START_X + (index * SLOT_SPACING);
}

static lv_coord_t get_slot_y(int index) {
    // Removed the staggering effect for cleaner large UI
    return START_Y;
}

static void refresh_dashboard(void);
static void toggle_delete_mode(void);
static void toggle_rename_mode(void);

/* ---------------- Visual Updates ---------------- */

static void fill_slot_with_data(slot_handles_t* h, const plot_data_t* data) {
    if (!data) {
        lv_obj_add_flag(h->container, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(h->container, LV_OBJ_FLAG_HIDDEN);

    int32_t track_h = TRACK_HEIGHT;
    int32_t track_y = TRACK_Y_OFFSET;

    /* --- VISUAL MODE HANDLING --- */
    if (delete_mode) {
        lv_obj_set_style_outline_width(h->container, 5, 0);
        lv_obj_set_style_outline_color(h->container, lv_color_hex(0xFF5555), 0);
        lv_obj_set_style_outline_pad(h->container, 0, 0);
        lv_obj_set_style_bg_color(h->container, lv_color_hex(0x442222), 0);
        lv_obj_set_style_text_color(h->label_name, lv_color_hex(0xFF9999), 0);
    }
    else if (rename_mode) {
        lv_obj_set_style_outline_width(h->container, 5, 0);
        lv_obj_set_style_outline_color(h->container, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_outline_pad(h->container, 0, 0);
        lv_obj_set_style_bg_color(h->container, lv_color_hex(0x224422), 0);
        lv_obj_set_style_text_color(h->label_name, lv_color_hex(0x99FF99), 0);
    }
    else {
        lv_obj_set_style_outline_width(h->container, 0, 0);
        lv_obj_set_style_bg_color(h->container, lv_color_hex(0x2B2B2B), 0);
        lv_obj_set_style_text_color(h->label_name, lv_color_white(), 0);
    }

    lv_label_set_text(h->label_name, data->name);

    if (edit_mode) {
        lv_label_set_text_fmt(h->label_percent, "%d%%", (int)data->threshold);
        lv_obj_set_style_text_color(h->label_percent, lv_color_hex(0xFF5555), 0);
        lv_obj_clear_flag(h->slider, LV_OBJ_FLAG_HIDDEN);
        if (lv_slider_get_value(h->slider) != data->threshold) {
            lv_slider_set_value(h->slider, data->threshold, LV_ANIM_OFF);
        }
    }
    else {
        lv_label_set_text_fmt(h->label_percent, "%d%%", (int)data->moisture);
        lv_obj_set_style_text_color(h->label_percent, lv_color_white(), 0);
        lv_obj_add_flag(h->slider, LV_OBJ_FLAG_HIDDEN);
    }

    int32_t val = data->moisture;
    if (val < 0) val = 0; if (val > 100) val = 100;

    int32_t cover_percent = 100 - val;
    if (cover_percent > 0) {
        lv_obj_set_height(h->cover, LV_PCT(cover_percent));
        lv_obj_clear_flag(h->cover, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_set_height(h->cover, 0);
        lv_obj_add_flag(h->cover, LV_OBJ_FLAG_HIDDEN);
    }

    /* Update Threshold Line */
    int32_t line_px_from_bottom = (track_h * data->threshold) / 100;
    lv_obj_set_y(h->top_line, track_y + track_h - line_px_from_bottom - 3);
}

/* ---------------- Button Fade Helpers ---------------- */

static void anim_opa_cb(void* var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t*)var, v, 0);
}

static void anim_hide_cb(lv_anim_t* a) {
    lv_obj_t* btn = (lv_obj_t*)a->var;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

static void update_button_visibility(lv_obj_t* btn, bool show) {
    bool is_hidden = lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN);
    if (show && is_hidden) {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(btn, 0, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, btn);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_exec_cb(&a, anim_opa_cb);
        lv_anim_set_time(&a, ANIM_TIME);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    else if (!show && !is_hidden) {
        int32_t current_opa = lv_obj_get_style_opa(btn, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, btn);
        lv_anim_set_values(&a, current_opa, 0);
        lv_anim_set_exec_cb(&a, anim_opa_cb);
        lv_anim_set_time(&a, ANIM_TIME);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&a, anim_hide_cb);
        lv_anim_start(&a);
    }
}

static void refresh_dashboard(void) {
    for (int i = 0; i < VISIBLE_SLOTS; i++) {
        int data_idx = scroll_offset + i;
        if (data_idx < plot_count) {
            fill_slot_with_data(ui_slots[i], &all_plots[data_idx]);
        }
        else {
            fill_slot_with_data(ui_slots[i], NULL);
        }
    }

    bool show_left = (scroll_offset > 0);
    bool show_right = (scroll_offset + VISIBLE_SLOTS < plot_count);

    update_button_visibility(btn_left, show_left);
    update_button_visibility(btn_right, show_right);
}

/* ---------------- Reset Logic ---------------- */

static void reset_confirm_event_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* mbox = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t* label = lv_obj_get_child(btn, 0);
    const char* txt = lv_label_get_text(label);

    if (txt && strcmp(txt, "Yes") == 0) {
        plot_count = 0;
        plot_name_counter = 0;
        scroll_offset = 0;
        /* Re-create Defaults */
        add_new_plot(); add_new_plot(); add_new_plot(); add_new_plot();
        all_plots[0].threshold = 80; all_plots[0].moisture = 50;
        all_plots[0].sim_target = 50; all_plots[0].sim_step = 0;
        all_plots[1].threshold = 20; all_plots[1].moisture = 80;
        all_plots[1].sim_target = 80; all_plots[1].sim_step = 1;
        all_plots[2].threshold = 95; all_plots[2].moisture = 100;
        all_plots[2].sim_target = 100; all_plots[2].sim_step = 0;
        all_plots[3].threshold = 50; all_plots[3].moisture = 10;
        all_plots[3].sim_target = 10; all_plots[3].sim_step = 1;

        if (delete_mode) toggle_delete_mode();
        if (rename_mode) toggle_rename_mode();
        refresh_dashboard();
        save_plots_to_disk();
    }
    lv_msgbox_close(mbox);
}

static void create_reset_popup(void)
{
    lv_obj_t* mbox = lv_msgbox_create(NULL);
    lv_obj_set_size(mbox, 520, 260);
    lv_obj_center(mbox);

    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(mbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mbox, 16, 0);
    lv_obj_set_style_border_color(mbox, lv_color_hex(0xFF5555), 0);
    lv_obj_set_style_border_width(mbox, 3, 0);
    lv_obj_set_style_shadow_width(mbox, 40, 0);

    /* ---------- TEXT CONTENT ---------- */
    lv_obj_t* content = lv_msgbox_get_content(mbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);

    lv_obj_set_style_pad_top(content, 24, 0);
    lv_obj_set_style_pad_bottom(content, 24, 0);
    lv_obj_set_style_pad_left(content, 32, 0);
    lv_obj_set_style_pad_right(content, 32, 0);
    lv_obj_set_style_pad_row(content, 8, 0);

    lv_obj_t* title = lv_label_create(content);
    lv_label_set_text(title, "FACTORY RESET");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF5555), 0);

    lv_obj_t* txt = lv_label_create(content);
    lv_label_set_text(txt, "Delete ALL plots and reset to default settings?");
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);
    lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_20, 0);

    /* make sure it does not wrap */
    lv_label_set_long_mode(txt, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(mbox, 560, 260);


    /* ---------- FOOTER BUTTONS ---------- */
    lv_obj_t* btn_yes = lv_msgbox_add_footer_button(mbox, "Yes");
    lv_obj_t* btn_no = lv_msgbox_add_footer_button(mbox, "No");

    lv_obj_t* footer = lv_obj_get_parent(btn_yes);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(footer, 32, 0);
    lv_obj_set_style_pad_right(footer, 32, 0);
    lv_obj_set_style_pad_top(footer, 12, 0);
    lv_obj_set_style_pad_bottom(footer, 16, 0);
    lv_obj_set_style_pad_column(footer, 16, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer,
        LV_FLEX_ALIGN_END,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_set_height(btn_yes, 56);
    lv_obj_set_style_pad_left(btn_yes, 28, 0);
    lv_obj_set_style_pad_right(btn_yes, 28, 0);
    lv_obj_set_style_radius(btn_yes, 10, 0);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_text_color(btn_yes, lv_color_white(), 0);

    lv_obj_set_height(btn_no, 56);
    lv_obj_set_style_pad_left(btn_no, 28, 0);
    lv_obj_set_style_pad_right(btn_no, 28, 0);
    lv_obj_set_style_radius(btn_no, 10, 0);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_color(btn_no, lv_color_white(), 0);

    lv_obj_add_event_cb(btn_yes, reset_confirm_event_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(btn_no, reset_confirm_event_cb, LV_EVENT_CLICKED, mbox);
}


/* ---------------- Deletion Logic ---------------- */

static void delete_confirm_event_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target(e);
    lv_obj_t* mbox_ref = (lv_obj_t*)lv_event_get_user_data(e);
    lv_obj_t* label = lv_obj_get_child(btn, 0);
    const char* txt = lv_label_get_text(label);

    if (txt && strcmp(txt, "Yes") == 0 && pending_delete_idx >= 0) {
        for (int i = pending_delete_idx; i < plot_count - 1; i++) {
            all_plots[i] = all_plots[i + 1];
        }
        plot_count--;
        if (scroll_offset > 0 && scroll_offset > plot_count - VISIBLE_SLOTS) {
            scroll_offset = (plot_count > VISIBLE_SLOTS) ? (plot_count - VISIBLE_SLOTS) : 0;
        }
        refresh_dashboard();
        save_plots_to_disk();
    }
    lv_msgbox_close(mbox_ref);
    pending_delete_idx = -1;
}

static void create_delete_popup(int data_idx)
{
    pending_delete_idx = data_idx;

    lv_obj_t* mbox = lv_msgbox_create(NULL);
    lv_obj_set_size(mbox, 520, 260);
    lv_obj_center(mbox);

    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(mbox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mbox, 16, 0);
    lv_obj_set_style_border_width(mbox, 0, 0);
    lv_obj_set_style_shadow_width(mbox, 40, 0);

    /* CONTENT (title + text) */
    lv_obj_t* content = lv_msgbox_get_content(mbox);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
        LV_FLEX_ALIGN_START,    /* vertical: top-down   */
        LV_FLEX_ALIGN_START,    /* horizontal: left     */
        LV_FLEX_ALIGN_START);

    lv_obj_set_style_pad_top(content, 24, 0);
    lv_obj_set_style_pad_bottom(content, 24, 0);
    lv_obj_set_style_pad_left(content, 32, 0);
    lv_obj_set_style_pad_right(content, 32, 0);
    lv_obj_set_style_pad_row(content, 8, 0);

    lv_obj_t* title_lbl = lv_label_create(content);
    lv_label_set_text(title_lbl, "Delete Plot");
    lv_obj_set_style_text_color(title_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, 0);

    lv_obj_t* txt = lv_label_create(content);
    lv_label_set_text(txt, "Are you sure you want to delete this plot?");
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(txt, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(txt, LV_TEXT_ALIGN_LEFT, 0);

    /* FOOTER BUTTONS (msgbox built‑in row at bottom) */
    lv_obj_t* btn_yes = lv_msgbox_add_footer_button(mbox, "Yes");
    lv_obj_t* btn_no = lv_msgbox_add_footer_button(mbox, "No");

    /* make the footer row fill width with side/bottom margins */
    lv_obj_t* footer = lv_obj_get_parent(btn_yes);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(footer, 32, 0);
    lv_obj_set_style_pad_right(footer, 32, 0);
    lv_obj_set_style_pad_top(footer, 12, 0);
    lv_obj_set_style_pad_bottom(footer, 16, 0);
    lv_obj_set_style_pad_column(footer, 16, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer,
        LV_FLEX_ALIGN_END,      /* buttons right‑aligned */
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);

    lv_obj_set_height(btn_yes, 56);
    lv_obj_set_style_pad_left(btn_yes, 28, 0);
    lv_obj_set_style_pad_right(btn_yes, 28, 0);
    lv_obj_set_style_radius(btn_yes, 10, 0);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(0xFF5555), 0);

    lv_obj_set_height(btn_no, 56);
    lv_obj_set_style_pad_left(btn_no, 28, 0);
    lv_obj_set_style_pad_right(btn_no, 28, 0);
    lv_obj_set_style_radius(btn_no, 10, 0);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(0x555555), 0);

    lv_obj_add_event_cb(btn_yes, delete_confirm_event_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(btn_no, delete_confirm_event_cb, LV_EVENT_CLICKED, mbox);
}




/* ---------------- Rename Logic ---------------- */

static void close_rename_popup(void) {
    if (rename_mbox) {
        lv_obj_delete(rename_mbox);
        rename_mbox = NULL;
        rename_ta = NULL;
        rename_kb = NULL;
    }
    pending_rename_idx = -1;
}

static void rename_kb_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        if (pending_rename_idx >= 0 && rename_ta) {
            const char* txt = lv_textarea_get_text(rename_ta);
            if (strlen(txt) > 0) {
                snprintf(all_plots[pending_rename_idx].name, 32, "%s", txt);
                refresh_dashboard();
                save_plots_to_disk();
            }
        }
        close_rename_popup();
    }
    else if (code == LV_EVENT_CANCEL) {
        close_rename_popup();
    }
}

static void create_rename_popup(int data_idx) {
    pending_rename_idx = data_idx;
    rename_mbox = lv_obj_create(lv_screen_active());
    lv_obj_set_size(rename_mbox, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(rename_mbox, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(rename_mbox, LV_OPA_70, 0);
    lv_obj_set_style_border_width(rename_mbox, 0, 0);
    lv_obj_add_flag(rename_mbox, LV_OBJ_FLAG_CLICKABLE);

    rename_ta = lv_textarea_create(rename_mbox);
    lv_textarea_set_one_line(rename_ta, true);
    lv_textarea_set_text(rename_ta, all_plots[data_idx].name);
    lv_textarea_set_max_length(rename_ta, 12);
    lv_obj_align(rename_ta, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_width(rename_ta, 400);
    lv_obj_set_height(rename_ta, 60);

    rename_kb = lv_keyboard_create(rename_mbox);
    lv_keyboard_set_textarea(rename_kb, rename_ta);
    lv_obj_add_event_cb(rename_kb, rename_kb_event_cb, LV_EVENT_ALL, NULL);

    /* Make keyboard larger */
    lv_obj_set_height(rename_kb, LV_PCT(50));

    lv_obj_send_event(rename_ta, LV_EVENT_FOCUSED, NULL);
}

/* ---------------- Event Handlers ---------------- */

static void toggle_delete_mode(void) {
    delete_mode = !delete_mode;
    if (delete_mode) {
        rename_mode = false;
        lv_obj_set_style_bg_color(btn_rename, lv_color_hex(0x2B2B2B), 0);
        lv_obj_set_style_text_color(btn_rename, lv_color_white(), 0);
        lv_obj_set_style_bg_color(btn_del, lv_color_hex(0xFF5555), 0);
        lv_obj_set_style_text_color(btn_del, lv_color_white(), 0);
    }
    else {
        lv_obj_set_style_bg_color(btn_del, lv_color_hex(0x2B2B2B), 0);
        lv_obj_set_style_text_color(btn_del, lv_color_white(), 0);
    }
    refresh_dashboard();
}

static void toggle_rename_mode(void) {
    rename_mode = !rename_mode;
    if (rename_mode) {
        delete_mode = false;
        lv_obj_set_style_bg_color(btn_del, lv_color_hex(0x2B2B2B), 0);
        lv_obj_set_style_text_color(btn_del, lv_color_white(), 0);
        lv_obj_set_style_bg_color(btn_rename, lv_color_hex(0x00AA00), 0);
        lv_obj_set_style_text_color(btn_rename, lv_color_white(), 0);
    }
    else {
        lv_obj_set_style_bg_color(btn_rename, lv_color_hex(0x2B2B2B), 0);
        lv_obj_set_style_text_color(btn_rename, lv_color_white(), 0);
    }
    refresh_dashboard();
}

static void slot_click_event_cb(lv_event_t* e) {
    if (!delete_mode && !rename_mode) return;

    lv_obj_t* target = lv_event_get_target(e);
    int slot_idx = -1;

    for (int i = 0; i < VISIBLE_SLOTS; i++) {
        if (ui_slots[i]->container == target || ui_slots[i]->slider == target) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx == -1) return;

    int data_idx = scroll_offset + slot_idx;
    if (data_idx >= plot_count) return;

    if (delete_mode) create_delete_popup(data_idx);
    else if (rename_mode) create_rename_popup(data_idx);
}

static void slider_event_cb(lv_event_t* e) {
    lv_obj_t* slider = lv_event_get_target(e);
    if (is_animating) return;

    int slot_idx = -1;
    for (int i = 0; i < VISIBLE_SLOTS; i++) {
        if (ui_slots[i]->slider == slider) {
            slot_idx = i;
            break;
        }
    }
    if (slot_idx == -1) return;

    if (delete_mode || rename_mode) {
        /* If in a mode where slider interaction is forbidden, revert */
        int data_idx = scroll_offset + slot_idx;
        if (data_idx < plot_count) {
            lv_slider_set_value(ui_slots[slot_idx]->slider, all_plots[data_idx].threshold, LV_ANIM_OFF);
        }
        return;
    }

    int data_idx = scroll_offset + slot_idx;
    if (data_idx >= plot_count) return;

    all_plots[data_idx].threshold = lv_slider_get_value(slider);
    fill_slot_with_data(ui_slots[slot_idx], &all_plots[data_idx]);
    save_plots_to_disk();
}

static void edit_button_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        edit_mode = !edit_mode;

        if (!edit_mode) {
            if (delete_mode) toggle_delete_mode();
            if (rename_mode) toggle_rename_mode();
        }

        if (edit_mode) {
            lv_obj_clear_flag(btn_add, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(btn_del, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(btn_rename, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(btn_edit_label, LV_SYMBOL_CLOSE);
        }
        else {
            lv_obj_add_flag(btn_add, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_del, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(btn_rename, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(btn_edit_label, LV_SYMBOL_EDIT);
        }
        refresh_dashboard();
    }
}

static void delete_button_event_cb(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        toggle_delete_mode();
    }
    else if (code == LV_EVENT_LONG_PRESSED) {
        create_reset_popup();
    }
}

static void rename_button_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) toggle_rename_mode();
}

static void add_button_event_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        add_new_plot();
        refresh_dashboard();
        save_plots_to_disk();
    }
}

/* ---------------- Animation + Scroll ---------------- */

static void anim_x_cb(void* var, int32_t v) {
    lv_obj_set_x((lv_obj_t*)var, v);
}

static void anim_ready_cb(lv_anim_t* a) {
    is_animating = false;
    refresh_dashboard();
}

static void animate_slide(int direction) {
    if (is_animating) return;
    is_animating = true;

    /*
       With 1280x800, we don't use Y staggering.
       Everything moves purely horizontally.
    */

    lv_anim_t a_cb;
    lv_anim_init(&a_cb);
    lv_anim_set_exec_cb(&a_cb, anim_x_cb);
    lv_anim_set_time(&a_cb, ANIM_TIME);
    lv_anim_set_path_cb(&a_cb, lv_anim_path_ease_out);

    if (direction == 1) { // Moving RIGHT (showing content from right)
        scroll_offset++;

        slot_handles_t* new_right_slot = spare_slot;
        int new_data_idx = scroll_offset + 3;

        fill_slot_with_data(new_right_slot, (new_data_idx < plot_count) ? &all_plots[new_data_idx] : NULL);

        lv_coord_t start_x = get_slot_x(3) + SLOT_SPACING;
        lv_obj_set_x(new_right_slot->container, start_x);
        lv_obj_set_y(new_right_slot->container, START_Y);
        lv_obj_set_style_opa(new_right_slot->container, 255, 0);
        lv_obj_clear_flag(new_right_slot->container, LV_OBJ_FLAG_HIDDEN);

        /* Animate Current Slots Left */
        for (int i = 0; i < 4; i++) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ui_slots[i]->container);
            lv_anim_set_values(&a, get_slot_x(i), get_slot_x(i) - SLOT_SPACING);
            lv_anim_set_exec_cb(&a, anim_x_cb);
            lv_anim_set_time(&a, ANIM_TIME);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);

            if (i == 0) { // Fade out first slot
                lv_anim_t f;
                lv_anim_init(&f);
                lv_anim_set_var(&f, ui_slots[i]->container);
                lv_anim_set_values(&f, 255, 0);
                lv_anim_set_exec_cb(&f, anim_opa_cb);
                lv_anim_set_time(&f, ANIM_TIME);
                lv_anim_start(&f);
            }
        }

        // Animate new slot in
        lv_anim_set_var(&a_cb, new_right_slot->container);
        lv_anim_set_values(&a_cb, start_x, get_slot_x(3));
        lv_anim_set_completed_cb(&a_cb, anim_ready_cb);
        lv_anim_start(&a_cb);

        // Fade new slot in
        lv_obj_set_style_opa(new_right_slot->container, 255, 0);

        spare_slot = ui_slots[0];
        ui_slots[0] = ui_slots[1];
        ui_slots[1] = ui_slots[2];
        ui_slots[2] = ui_slots[3];
        ui_slots[3] = new_right_slot;

    }
    else { // Moving LEFT
        scroll_offset--;

        slot_handles_t* new_left_slot = spare_slot;
        int new_data_idx = scroll_offset;

        fill_slot_with_data(new_left_slot, &all_plots[new_data_idx]);

        lv_coord_t start_x = get_slot_x(0) - SLOT_SPACING;
        lv_obj_set_x(new_left_slot->container, start_x);
        lv_obj_set_y(new_left_slot->container, START_Y);
        lv_obj_set_style_opa(new_left_slot->container, 255, 0);
        lv_obj_clear_flag(new_left_slot->container, LV_OBJ_FLAG_HIDDEN);

        /* Animate Current Slots Right */
        for (int i = 0; i < 4; i++) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ui_slots[i]->container);
            lv_anim_set_values(&a, get_slot_x(i), get_slot_x(i) + SLOT_SPACING);
            lv_anim_set_exec_cb(&a, anim_x_cb);
            lv_anim_set_time(&a, ANIM_TIME);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);

            if (i == 3) { // Fade out last slot
                lv_anim_t f;
                lv_anim_init(&f);
                lv_anim_set_var(&f, ui_slots[i]->container);
                lv_anim_set_values(&f, 255, 0);
                lv_anim_set_exec_cb(&f, anim_opa_cb);
                lv_anim_set_time(&f, ANIM_TIME);
                lv_anim_start(&f);
            }
        }

        lv_anim_set_var(&a_cb, new_left_slot->container);
        lv_anim_set_values(&a_cb, start_x, get_slot_x(0));
        lv_anim_set_completed_cb(&a_cb, anim_ready_cb);
        lv_anim_start(&a_cb);

        spare_slot = ui_slots[3];
        ui_slots[3] = ui_slots[2];
        ui_slots[2] = ui_slots[1];
        ui_slots[1] = ui_slots[0];
        ui_slots[0] = new_left_slot;
    }
}

static void scroll_left_btn_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (is_animating) return;
        if (scroll_offset > 0) animate_slide(-1);
    }
}

static void scroll_right_btn_cb(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (is_animating) return;
        if (scroll_offset + VISIBLE_SLOTS < plot_count) animate_slide(1);
    }
}

static void sensor_simulation_timer_cb(lv_timer_t* timer) {
    (void)timer;
    for (int i = 0; i < plot_count; i++) {
        plot_data_t* p = &all_plots[i];
        if (p->moisture == p->sim_target) {
            p->sim_target = rand() % 101;
            p->sim_step = (p->sim_target > p->moisture) ? 1 : -1;
        }

        if (p->moisture != p->sim_target) {
            p->moisture += p->sim_step;
        }
    }
    if (!is_animating) refresh_dashboard();
}

/* ---------------- UI Construction ---------------- */

static void init_single_slot(slot_handles_t* h, lv_obj_t* parent, lv_coord_t x, lv_coord_t y) {
    h->container = lv_obj_create(parent);
    lv_obj_set_pos(h->container, x, y);
    lv_obj_set_size(h->container, SLOT_WIDTH, SLOT_HEIGHT);
    lv_obj_set_style_bg_color(h->container, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(h->container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h->container, 16, 0);
    lv_obj_set_style_border_width(h->container, 0, 0);
    lv_obj_set_style_pad_all(h->container, 0, 0);
    lv_obj_remove_flag(h->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h->container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->container, slot_click_event_cb, LV_EVENT_CLICKED, NULL);

    /* Percent Label */
    h->label_percent = lv_label_create(h->container);
    lv_obj_set_style_text_color(h->label_percent, lv_color_white(), 0);
    lv_obj_set_style_text_font(h->label_percent, &lv_font_montserrat_20, 0); /* Use standard large if available */
    lv_obj_align(h->label_percent, LV_ALIGN_TOP_MID, 0, 20);

    /* Track & Gradient */
    lv_obj_t* track = lv_obj_create(h->container);

    // Centering the track: (SLOT_WIDTH - TRACK_WIDTH) / 2
    lv_coord_t track_x = (SLOT_WIDTH - TRACK_WIDTH) / 2;

    lv_obj_set_pos(track, track_x, TRACK_Y_OFFSET);
    lv_obj_set_size(track, TRACK_WIDTH, TRACK_HEIGHT);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x353535), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(track, 8, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* grad_top = lv_obj_create(track);
    lv_obj_remove_style_all(grad_top);
    lv_obj_set_size(grad_top, TRACK_WIDTH, (TRACK_HEIGHT / 2) + 2);  // Overlap
    lv_obj_set_align(grad_top, LV_ALIGN_TOP_MID);
    lv_obj_set_style_radius(grad_top, 0, 0);      // ← FIXED: No radius
    lv_obj_set_style_bg_grad(grad_top, &g_top, 0);
    lv_obj_set_style_bg_opa(grad_top, LV_OPA_COVER, 0);

    lv_obj_t* grad_btm = lv_obj_create(track);
    lv_obj_remove_style_all(grad_btm);
    lv_obj_set_size(grad_btm, TRACK_WIDTH, (TRACK_HEIGHT / 2) + 2);  // Overlap
    lv_obj_set_align(grad_btm, LV_ALIGN_BOTTOM_MID);
    lv_obj_set_style_radius(grad_btm, 0, 0);      // ← FIXED: No radius
    lv_obj_set_style_bg_grad(grad_btm, &g_btm, 0);
    lv_obj_set_style_bg_opa(grad_btm, LV_OPA_COVER, 0);


    /* Cover (Grey overlay) */
    h->cover = lv_obj_create(track);
    lv_obj_set_width(h->cover, TRACK_WIDTH);
    lv_obj_set_align(h->cover, LV_ALIGN_TOP_MID);
    lv_obj_set_style_bg_color(h->cover, lv_color_hex(0x353535), 0);
    lv_obj_set_style_bg_opa(h->cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(h->cover, 0, 0);
    lv_obj_set_style_radius(h->cover, 0, 0);
    lv_obj_remove_flag(h->cover, LV_OBJ_FLAG_SCROLLABLE);

    /* Threshold Line */
    h->top_line = lv_obj_create(h->container);
    lv_obj_set_size(h->top_line, TRACK_WIDTH + 10, 6); // Slightly wider and thicker
    lv_obj_set_x(h->top_line, track_x - 5);
    lv_obj_set_style_bg_color(h->top_line, lv_color_hex(0xFF0000), 0);
    lv_obj_set_style_bg_opa(h->top_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(h->top_line, 0, 0);

    /* Name Label */
    h->label_name = lv_label_create(h->container);
    lv_obj_set_style_text_color(h->label_name, lv_color_white(), 0);
    lv_obj_set_style_text_font(h->label_name, &lv_font_montserrat_20, 0);
    lv_obj_align(h->label_name, LV_ALIGN_BOTTOM_MID, 0, -25);

    /* Slider (Invisible Interaction) */
    h->slider = lv_slider_create(h->container);
    lv_obj_set_size(h->slider, TRACK_WIDTH, TRACK_HEIGHT);
    lv_obj_set_pos(h->slider, track_x, TRACK_Y_OFFSET);
    lv_slider_set_range(h->slider, 0, 100);
    lv_obj_set_style_bg_opa(h->slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(h->slider, LV_OPA_TRANSP, LV_PART_INDICATOR);

    /* Knob Styling (Visible only in edit mode) */
    lv_obj_set_style_bg_color(h->slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(h->slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(h->slider, 8, LV_PART_KNOB);
    lv_obj_set_style_pad_all(h->slider, 0, LV_PART_KNOB);
    lv_obj_set_style_min_height(h->slider, 20, LV_PART_KNOB);
    lv_obj_set_style_min_width(h->slider, TRACK_WIDTH + 10, LV_PART_KNOB);

    lv_obj_add_flag(h->slider, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(h->slider, slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(h->slider, slot_click_event_cb, LV_EVENT_CLICKED, NULL);
}

void ui_moisture_dashboard_absolute(void) {
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x212121), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    init_gradients();

    /* Title */
    lv_obj_t* title = lv_label_create(scr);
    lv_label_set_text(title, "Moisture Data");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0); // Ideally use larger font
    lv_obj_set_style_text_color(title, lv_color_hex(0xA6A6A6), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Load from Disk OR Create Defaults */
    bool loaded = load_plots_from_disk();

    /* Slots Init */
    init_single_slot(&slots_storage[0], scr, get_slot_x(0), START_Y);
    init_single_slot(&slots_storage[1], scr, get_slot_x(1), START_Y);
    init_single_slot(&slots_storage[2], scr, get_slot_x(2), START_Y);
    init_single_slot(&slots_storage[3], scr, get_slot_x(3), START_Y);

    /* Spare Slot (offscreen) */
    init_single_slot(&slots_storage[4], scr, -500, 0);
    lv_obj_add_flag(slots_storage[4].container, LV_OBJ_FLAG_HIDDEN);

    ui_slots[0] = &slots_storage[0];
    ui_slots[1] = &slots_storage[1];
    ui_slots[2] = &slots_storage[2];
    ui_slots[3] = &slots_storage[3];
    spare_slot = &slots_storage[4];

    /* Scroll Buttons */
    int scroll_btn_y = (SCREEN_HEIGHT - 200) / 2;

    btn_left = lv_btn_create(scr);
    lv_obj_set_pos(btn_left, 20, scroll_btn_y);
    lv_obj_set_size(btn_left, 80, 200);
    lv_obj_set_style_bg_color(btn_left, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(btn_left, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_left, 12, 0);
    lv_obj_add_flag(btn_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_left, scroll_left_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* lbl_left = lv_label_create(btn_left);
    lv_label_set_text(lbl_left, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_left);

    btn_right = lv_btn_create(scr);
    lv_obj_set_pos(btn_right, SCREEN_WIDTH - 100, scroll_btn_y);
    lv_obj_set_size(btn_right, 80, 200);
    lv_obj_set_style_bg_color(btn_right, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(btn_right, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_right, 12, 0);
    lv_obj_add_flag(btn_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_right, scroll_right_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* lbl_right = lv_label_create(btn_right);
    lv_label_set_text(lbl_right, LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_right);

    /* ACTION BUTTONS (Corners) */

    /* ADD BUTTON (Top Right) */
    btn_add = lv_btn_create(scr);
    lv_obj_set_size(btn_add, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_align(btn_add, LV_ALIGN_TOP_RIGHT, -BUTTON_MARGIN, BUTTON_MARGIN);
    lv_obj_set_style_radius(btn_add, 12, 0);
    lv_obj_set_style_bg_color(btn_add, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(btn_add, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn_add, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_add, add_button_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t* lbl_plus = lv_label_create(btn_add);
    lv_label_set_text(lbl_plus, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_plus);

    /* DELETE BUTTON (Top Left) */
    btn_del = lv_btn_create(scr);
    lv_obj_set_size(btn_del, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_align(btn_del, LV_ALIGN_TOP_LEFT, BUTTON_MARGIN, BUTTON_MARGIN);
    lv_obj_set_style_radius(btn_del, 12, 0);
    lv_obj_set_style_bg_color(btn_del, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(btn_del, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn_del, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_del, delete_button_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t* lbl_minus = lv_label_create(btn_del);
    lv_label_set_text(lbl_minus, LV_SYMBOL_MINUS);
    lv_obj_center(lbl_minus);

    /* RENAME BUTTON (Bottom Left) */
    btn_rename = lv_btn_create(scr);
    lv_obj_set_size(btn_rename, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_align(btn_rename, LV_ALIGN_BOTTOM_LEFT, BUTTON_MARGIN, -BUTTON_MARGIN);
    lv_obj_set_style_radius(btn_rename, 12, 0);
    lv_obj_set_style_bg_color(btn_rename, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(btn_rename, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn_rename, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn_rename, rename_button_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_t* lbl_kb = lv_label_create(btn_rename);
    lv_label_set_text(lbl_kb, LV_SYMBOL_KEYBOARD);
    lv_obj_center(lbl_kb);

    /* EDIT BUTTON (Bottom Right) */
    btn_edit = lv_btn_create(scr);
    lv_obj_set_size(btn_edit, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_align(btn_edit, LV_ALIGN_BOTTOM_RIGHT, -BUTTON_MARGIN, -BUTTON_MARGIN);
    lv_obj_set_style_radius(btn_edit, 12, 0);
    lv_obj_set_style_bg_color(btn_edit, lv_color_hex(0x2B2B2B), 0);
    lv_obj_set_style_bg_opa(btn_edit, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(btn_edit, edit_button_event_cb, LV_EVENT_ALL, NULL);
    btn_edit_label = lv_label_create(btn_edit);
    lv_label_set_text(btn_edit_label, LV_SYMBOL_EDIT);
    lv_obj_center(btn_edit_label);

    if (!loaded) {
        add_new_plot(); add_new_plot(); add_new_plot(); add_new_plot();
        all_plots[0].threshold = 80; all_plots[0].moisture = 50;
        all_plots[1].threshold = 20; all_plots[1].moisture = 80;
        all_plots[2].threshold = 95; all_plots[2].moisture = 100;
        all_plots[3].threshold = 50; all_plots[3].moisture = 10;
        save_plots_to_disk();
    }

    refresh_dashboard();
    lv_timer_create(sensor_simulation_timer_cb, 50, NULL);
}
