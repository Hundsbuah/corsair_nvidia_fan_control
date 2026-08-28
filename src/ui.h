#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

/*
 * Dark "control deck" UI layer for the Win32 frontend.
 * All controls are custom-drawn; palette, fonts and brushes are dpi-aware.
 * Logic (devices, settings, tray) stays in main.c and only talks to the
 * standard window messages (WM_COMMAND, WM_HSCROLL, ...) plus the small
 * ui_* helpers below.
 */

#define UI_FONT_BODY 0      /* Segoe UI, normal      */
#define UI_FONT_BOLD 1      /* Segoe UI, semibold    */
#define UI_FONT_MONO 2      /* Consolas, normal      */
#define UI_FONT_MONO_BOLD 3 /* Consolas, semibold    */
#define UI_FONT_HEADING 4   /* Segoe UI, small bold  */
#define UI_FONT_CAPTION 5   /* Segoe UI, small       */
#define UI_FONT_VALUE 6     /* Consolas, large       */

#define UI_BTN_SUBTLE 0  /* panel background, edge border   */
#define UI_BTN_PRIMARY 1 /* accent fill, dark text          */

#define UI_DOT_OK 0
#define UI_DOT_WARN 1
#define UI_DOT_OFF 2

#define UI_BG_INK 0
#define UI_BG_PANEL 1

typedef struct UiTheme {
    int dpi;

    COLORREF ink;        /* window background          */
    COLORREF panel;      /* card background            */
    COLORREF panel_alt;  /* slider track / hover fill  */
    COLORREF row_alt;    /* alternate list rows        */
    COLORREF edge;       /* 1px borders, grid lines    */
    COLORREF edge_hi;    /* hover border               */
    COLORREF text;       /* primary text               */
    COLORREF dim;        /* secondary text, captions   */
    COLORREF accent;     /* brand blue                 */
    COLORREF accent_hi;  /* accent hover               */
    COLORREF ok;         /* green, online              */
    COLORREF warn;       /* amber, degraded            */
    COLORREF err;        /* red, failed                */

    HFONT font[7];
    HBRUSH ink_brush;
    HBRUSH panel_brush;
    HBRUSH panel_alt_brush;
    HBRUSH edge_brush;
    HBRUSH edge_hi_brush;
    HBRUSH dim_brush;
    HBRUSH accent_brush;
    HBRUSH accent_hi_brush;
} UiTheme;

/* Global lifecycle ------------------------------------------------------- */
void ui_init(void);                 /* register control classes, build theme */
void ui_destroy(void);              /* release fonts/brushes                 */
UiTheme *ui_theme(void);            /* current theme (dpi cached)            */
void ui_set_dpi(int dpi);           /* rebuild fonts and brushes             */
int ui_px(int v);                   /* scale a 96-dpi pixel value            */

/* Control classes (create with these class names + WS_CHILD) ------------- */
#define UI_CLASS_PANEL L"CfcPanel"
#define UI_CLASS_BUTTON L"CfcButton"
#define UI_CLASS_SLIDER L"CfcSlider"
#define UI_CLASS_COMBO L"CfcCombo"
#define UI_CLASS_CURVE L"CfcCurve"
#define UI_CLASS_DOT L"CfcDot"

HWND ui_make_control(HWND parent, const wchar_t *class_name, DWORD extra_style,
                     int x, int y, int w, int h, int id);

/* Buttons ---------------------------------------------------------------- */
void ui_button_set_variant(HWND hwnd, int variant);   /* UI_BTN_*            */

/* Sliders ---------------------------------------------------------------- */
int ui_slider_value(HWND hwnd);
void ui_slider_set(HWND hwnd, int value);             /* sends TB_POSITION   */

/* Combos (popup-menu style) ---------------------------------------------- */
void ui_combo_set_items(HWND hwnd, int count, const wchar_t *const *items);
void ui_combo_set_selected(HWND hwnd, int index);
int ui_combo_selected(HWND hwnd);

/* GPU two-point curve ------------------------------------------------------ */
void ui_curve_set_points(HWND hwnd, int temp_low, int duty_low,
                         int temp_high, int duty_high);
void ui_curve_set_now(HWND hwnd, int temperature_c, bool ok);

/* Status dot --------------------------------------------------------------- */
void ui_dot_set(HWND hwnd, int state);                /* UI_DOT_*            */

/* Color helpers ------------------------------------------------------------ */
COLORREF ui_status_color(int state);                  /* UI_DOT_* -> color   */

/* WM_CTLCOLOR* handling for native controls (edits, checkboxes, statics).
 * Call from the parent window procedure when the message is for a child.   */
HBRUSH ui_handle_ctrl_color(HWND parent, HDC hdc, HWND child);

/* Apply the theme font matching role to every child static/edit/checkbox.
 * Child font role must have been registered with ui_register_font_role.    */
void ui_register_font_role(HWND hwnd, int role);
void ui_register_ctrl(HWND hwnd, int bg, COLORREF color); /* color 0 = text  */
void ui_apply_fonts(HWND hwnd);                       /* re-send WM_SETFONT  */

/* Dark list view setup: custom-drawn rows and a dark header.
 * Call once after ListView creation (list must be created with LVS_REPORT
 * and as a child of a CfcPanel window). Row cells are drawn on demand
 * through text_fn via the panel's NM_CUSTOMDRAW handling; row backgrounds
 * and per-cell text colors follow the dark theme. */
typedef void (*UiListTextFn)(void *ctx, int row, int col, wchar_t *text,
                             int text_count);
void ui_dark_listview(HWND list, unsigned numeric_mask, UiListTextFn text_fn,
                      void *ctx);

/* Attach a dark list view to a panel window. The list view must be created
 * as a child of the panel; the panel then answers the list view's
 * WM_NOTIFY (NM_CUSTOMDRAW) and WM_CTLCOLORLISTVIEW messages, which the
 * list view sends to its parent. */
void ui_panel_attach_list(HWND panel, HWND list);