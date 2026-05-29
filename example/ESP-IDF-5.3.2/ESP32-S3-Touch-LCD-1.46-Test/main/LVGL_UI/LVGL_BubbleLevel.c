/*
 * LVGL_BubbleLevel.c  v3
 * Draws directly into the canvas pixel buffer — no lv_canvas_draw calls
 * which were crashing due to incorrect usage.
 */
#include "LVGL_BubbleLevel.h"
#include "QMI8658.h"
#include "Display_SPD2010.h"
#include <math.h>
#include "esp_heap_caps.h"

#define CX      206
#define CY      206
#define CW      412
#define CH      412
#define BOWL_R  170
#define RING2_R 113
#define RING1_R  57
#define BUBBLE_R 34
#define MAX_ANG  30.0f
#define THRESH_LEVEL 1.5f
#define THRESH_TILT  12.0f
#define LPF_A   0.12f

static float lpf_pitch = 0, lpf_roll = 0;
static lv_color_t *cbuf = NULL;
static lv_obj_t *canvas  = NULL;
static lv_obj_t *lbl_x   = NULL;
static lv_obj_t *lbl_y   = NULL;
static lv_obj_t *lbl_st  = NULL;

/* ── pixel helpers ───────────────────────────────────────────────────────── */
static inline void put_px(int x, int y, lv_color_t c)
{
    if (x < 0 || x >= CW || y < 0 || y >= CH) return;
    cbuf[y * CW + x] = c;
}

static void draw_circle_outline(int cx, int cy, int r, lv_color_t c)
{
    int x = 0, y = r, d = 3 - 2 * r;
    while (y >= x) {
        put_px(cx+x,cy+y,c); put_px(cx-x,cy+y,c);
        put_px(cx+x,cy-y,c); put_px(cx-x,cy-y,c);
        put_px(cx+y,cy+x,c); put_px(cx-y,cy+x,c);
        put_px(cx+y,cy-x,c); put_px(cx-y,cy-x,c);
        if (d < 0) d += 4*x+6; else { d += 4*(x-y)+10; y--; }
        x++;
    }
}

static void draw_filled_circle(int cx, int cy, int r, lv_color_t c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                put_px(cx+dx, cy+dy, c);
}

static void draw_hline(int x0, int x1, int y, lv_color_t c)
{
    for (int x = x0; x <= x1; x++) put_px(x, y, c);
}

static void draw_vline(int x, int y0, int y1, lv_color_t c)
{
    for (int y = y0; y <= y1; y++) put_px(x, y, c);
}

/* ── main draw ───────────────────────────────────────────────────────────── */
static void redraw(float pitch, float roll)
{
    lv_color_t BG    = lv_color_make(0x0A,0x0A,0x0A);
    lv_color_t RING  = lv_color_make(0x2A,0x2A,0x2A);
    lv_color_t WHITE = lv_color_make(0xFF,0xFF,0xFF);
    lv_color_t GRAY  = lv_color_make(0x55,0x55,0x55);

    float fx = roll  / MAX_ANG;
    float fy = pitch / MAX_ANG;
    float mag = sqrtf(fx*fx + fy*fy);
    if (mag > 1.0f) { fx /= mag; fy /= mag; }
    float ang_deg = mag * MAX_ANG;

    lv_color_t BC = (ang_deg <= THRESH_LEVEL) ? lv_color_make(0x00,0xC8,0x6E) :
                    (ang_deg <= THRESH_TILT)  ? lv_color_make(0xFF,0xAA,0x00) :
                                                lv_color_make(0xE0,0x30,0x30);

    /* clear */
    for (int i = 0; i < CW*CH; i++) cbuf[i] = BG;

    /* rings */
    draw_circle_outline(CX, CY, BOWL_R,  RING);
    draw_circle_outline(CX, CY, RING2_R, RING);
    draw_circle_outline(CX, CY, RING1_R, RING);

    /* crosshairs */
    draw_hline(CX-BOWL_R, CX+BOWL_R, CY, RING);
    draw_vline(CX, CY-BOWL_R, CY+BOWL_R, RING);

    /* bubble */
    int bx = CX + (int)(fx * (BOWL_R - BUBBLE_R - 4));
    int by = CY + (int)(fy * (BOWL_R - BUBBLE_R - 4));
    draw_filled_circle(bx, by, BUBBLE_R, BC);
    draw_circle_outline(bx, by, BUBBLE_R, WHITE);
    /* glint */
    draw_filled_circle(bx - BUBBLE_R/4, by - BUBBLE_R/4, BUBBLE_R/5,
                       lv_color_make(0xFF,0xFF,0xFF));

    /* centre dot */
    draw_filled_circle(CX, CY, 5, GRAY);

    lv_canvas_set_buffer(canvas, cbuf, CW, CH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(canvas);
}

/* ── timer ───────────────────────────────────────────────────────────────── */
static void level_timer_cb(lv_timer_t *t)
{
    (void)t;
    float ax = Accel.x, ay = Accel.y, az = Accel.z;

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
        lv_obj_set_style_text_color(lbl_st, lv_color_make(0x00,0xC8,0x6E), 0);
    } else if (m <= THRESH_TILT) {
        lv_label_set_text(lbl_st, "TILT");
        lv_obj_set_style_text_color(lbl_st, lv_color_make(0xFF,0xAA,0x00), 0);
    } else {
        lv_label_set_text(lbl_st, "STEEP");
        lv_obj_set_style_text_color(lbl_st, lv_color_make(0xE0,0x30,0x30), 0);
    }
}

/* ── public ──────────────────────────────────────────────────────────────── */
void BubbleLevel_Show(void)
{
    cbuf = (lv_color_t *)heap_caps_malloc(CW * CH * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!cbuf) cbuf = (lv_color_t *)heap_caps_malloc(CW * CH * sizeof(lv_color_t), MALLOC_CAP_8BIT);
    assert(cbuf);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_make(0x0A,0x0A,0x0A), 0);

    canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, cbuf, CW, CH, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, LV_ALIGN_CENTER, 0, 0);

    lbl_st = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_st, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_st, lv_color_make(0x00,0xC8,0x6E), 0);
    lv_label_set_text(lbl_st, "LEVEL");
    lv_obj_align(lbl_st, LV_ALIGN_TOP_MID, 0, 52);

    lbl_x = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_x, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_x, lv_color_make(0xFF,0xFF,0xFF), 0);
    lv_label_set_text(lbl_x, "X  0.0");
    lv_obj_align(lbl_x, LV_ALIGN_BOTTOM_LEFT, 20, -14);

    lbl_y = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_y, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_y, lv_color_make(0xFF,0xFF,0xFF), 0);
    lv_label_set_text(lbl_y, "Y  0.0");
    lv_obj_align(lbl_y, LV_ALIGN_BOTTOM_RIGHT, -20, -14);

    redraw(0, 0);
    lv_timer_create(level_timer_cb, 33, NULL);
}
