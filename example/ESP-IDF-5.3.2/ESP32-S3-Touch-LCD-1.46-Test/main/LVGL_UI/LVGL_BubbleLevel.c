/*
 * LVGL_BubbleLevel.c
 * Fullscreen bubble level using the QMI8658 accelerometer.
 * Canvas buffer allocated in PSRAM to avoid stack overflow.
 */
#include "LVGL_BubbleLevel.h"
#include "QMI8658.h"
#include "Display_SPD2010.h"
#include <math.h>
#include "esp_heap_caps.h"

/* ── geometry ────────────────────────────────────────────────────────────── */
#define CX          206
#define CY          206
#define BOWL_R      170
#define RING2_R     113
#define RING1_R      57
#define BUBBLE_R     34
#define MAX_ANG      30.0f

/* ── thresholds ──────────────────────────────────────────────────────────── */
#define THRESH_LEVEL   1.5f
#define THRESH_TILT   12.0f

/* ── LPF ─────────────────────────────────────────────────────────────────── */
#define LPF_A   0.12f
static float lpf_pitch = 0, lpf_roll = 0;

/* ── canvas buffer allocated in PSRAM at runtime ────────────────────────── */
#define CW 412
#define CH 412
static lv_color_t *cbuf = NULL;

/* ── LVGL objects ────────────────────────────────────────────────────────── */
static lv_obj_t *canvas   = NULL;
static lv_obj_t *lbl_x    = NULL;
static lv_obj_t *lbl_y    = NULL;
static lv_obj_t *lbl_st   = NULL;

/* ── colours ─────────────────────────────────────────────────────────────── */
#define C_BG     lv_color_make(0x0A,0x0A,0x0A)
#define C_RING   lv_color_make(0x2A,0x2A,0x2A)
#define C_GREEN  lv_color_make(0x00,0xC8,0x6E)
#define C_AMBER  lv_color_make(0xFF,0xAA,0x00)
#define C_RED    lv_color_make(0xE0,0x30,0x30)
#define C_WHITE  lv_color_make(0xFF,0xFF,0xFF)
#define C_GRAY   lv_color_make(0x44,0x44,0x44)

/* ─────────────────────────────────────────────────────────────────────────
 * Draw
 * ───────────────────────────────────────────────────────────────────────── */
static void redraw(float pitch, float roll)
{
    lv_canvas_fill_bg(canvas, C_BG, LV_OPA_COVER);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);

    /* rings */
    arc.color = C_RING;
    arc.width = 1;
    int rings[] = {BOWL_R, RING2_R, RING1_R};
    for (int i = 0; i < 3; i++)
        lv_canvas_draw_arc(canvas, CX, CY, rings[i], 0, 360, &arc);

    /* crosshairs */
    line.color = C_RING; line.width = 1;
    lv_point_t h[] = {{CX-BOWL_R, CY},{CX+BOWL_R, CY}};
    lv_point_t v[] = {{CX, CY-BOWL_R},{CX, CY+BOWL_R}};
    lv_canvas_draw_line(canvas, h, 2, &line);
    lv_canvas_draw_line(canvas, v, 2, &line);

    /* bubble position */
    float fx = roll  / MAX_ANG;
    float fy = pitch / MAX_ANG;
    float mag = sqrtf(fx*fx + fy*fy);
    if (mag > 1.0f) { fx /= mag; fy /= mag; }

    int bx = CX + (int)(fx * (BOWL_R - BUBBLE_R - 4));
    int by = CY + (int)(fy * (BOWL_R - BUBBLE_R - 4));

    lv_color_t bc = (mag * MAX_ANG <= THRESH_LEVEL) ? C_GREEN :
                    (mag * MAX_ANG <= THRESH_TILT)  ? C_AMBER : C_RED;

    /* filled bubble */
    arc.color = bc; arc.width = BUBBLE_R;
    lv_canvas_draw_arc(canvas, bx, by, BUBBLE_R/2, 0, 360, &arc);
    /* outline */
    arc.color = C_WHITE; arc.width = 2;
    lv_canvas_draw_arc(canvas, bx, by, BUBBLE_R, 0, 360, &arc);
    /* glint */
    arc.color = C_WHITE; arc.width = BUBBLE_R/5;
    lv_canvas_draw_arc(canvas, bx-BUBBLE_R/4, by-BUBBLE_R/4, BUBBLE_R/5, 0, 360, &arc);

    /* centre dot */
    arc.color = C_GRAY; arc.width = 5;
    lv_canvas_draw_arc(canvas, CX, CY, 5, 0, 360, &arc);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Timer callback (~30 fps)
 * ───────────────────────────────────────────────────────────────────────── */
static void level_timer_cb(lv_timer_t *t)
{
    (void)t;
    float ax = getAccX(), ay = getAccY(), az = getAccZ();

    float pitch_r = atan2f(ay, sqrtf(ax*ax + az*az)) * (180.f / (float)M_PI);
    float roll_r  = atan2f(-ax, az)                  * (180.f / (float)M_PI);

    lpf_pitch += LPF_A * (pitch_r - lpf_pitch);
    lpf_roll  += LPF_A * (roll_r  - lpf_roll);

    redraw(lpf_pitch, lpf_roll);

    char buf[20];
    snprintf(buf, sizeof(buf), "X %+.1f", lpf_roll);
    lv_label_set_text(lbl_x, buf);
    snprintf(buf, sizeof(buf), "Y %+.1f", lpf_pitch);
    lv_label_set_text(lbl_y, buf);

    float m = sqrtf(lpf_pitch*lpf_pitch + lpf_roll*lpf_roll);
    if (m <= THRESH_LEVEL) {
        lv_label_set_text(lbl_st, "LEVEL");
        lv_obj_set_style_text_color(lbl_st, C_GREEN, 0);
    } else if (m <= THRESH_TILT) {
        lv_label_set_text(lbl_st, "TILT");
        lv_obj_set_style_text_color(lbl_st, C_AMBER, 0);
    } else {
        lv_label_set_text(lbl_st, "STEEP");
        lv_obj_set_style_text_color(lbl_st, C_RED, 0);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * Public
 * ───────────────────────────────────────────────────────────────────────── */
void BubbleLevel_Show(void)
{
    /* allocate canvas buffer in PSRAM */
    cbuf = (lv_color_t *)heap_caps_malloc(CW * CH * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!cbuf) {
        /* fallback to internal RAM if PSRAM fails */
        cbuf = (lv_color_t *)heap_caps_malloc(CW * CH * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, 0);

    canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, cbuf, CW, CH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);

    lbl_st = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_st, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_st, C_GREEN, 0);
    lv_label_set_text(lbl_st, "LEVEL");
    lv_obj_align(lbl_st, LV_ALIGN_TOP_MID, 0, 52);

    lbl_x = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_x, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_x, C_WHITE, 0);
    lv_label_set_text(lbl_x, "X  0.0");
    lv_obj_align(lbl_x, LV_ALIGN_BOTTOM_LEFT, 20, -14);

    lbl_y = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_y, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_y, C_WHITE, 0);
    lv_label_set_text(lbl_y, "Y  0.0");
    lv_obj_align(lbl_y, LV_ALIGN_BOTTOM_RIGHT, -20, -14);

    redraw(0, 0);
    lv_timer_create(level_timer_cb, 33, NULL);
}
