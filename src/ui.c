#include "ui.h"
#include "resource.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_CTLCOLORLISTVIEW
#define WM_CTLCOLORLISTVIEW 0x013E
#endif

/* ------------------------------------------------------------------ theme */

#define UI_LIST_MAX 4
#define UI_REGISTRY_MAX 256
#define UI_COMBO_ITEM_COUNT 8

typedef struct UiRegistryEntry {
    HWND hwnd;
    int role;       /* font role */
    int bg;         /* UI_BG_*   */
    COLORREF color; /* text color */
    bool has_font;
    bool has_ctrl;
} UiRegistryEntry;

static UiTheme g_theme;
static UiRegistryEntry g_registry[UI_REGISTRY_MAX];
static int g_list_count;

/* Forward declarations (window procedures). */
HWND ui_panel_list(HWND panel);
static LRESULT CALLBACK panel_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK button_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK slider_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK combo_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK curve_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK dot_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK listview_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK header_proc(HWND, UINT, WPARAM, LPARAM);

/* ------------------------------------------------------------------ theme */

static HFONT ui_create_font(const wchar_t *family, double points, DWORD weight,
                            int dpi)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight = (LONG)(points * dpi / 72.0 + 0.5);
    lf.lfHeight = -lf.lfHeight;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, family);
    return CreateFontIndirectW(&lf);
}

static void ui_release_fonts(void)
{
    for (int i = 0; i < 7; ++i) {
        if (g_theme.font[i]) {
            DeleteObject(g_theme.font[i]);
            g_theme.font[i] = NULL;
        }
    }
}

static void ui_release_brushes(void)
{
    HBRUSH brushes[8] = {
        g_theme.ink_brush, g_theme.panel_brush, g_theme.panel_alt_brush,
        g_theme.edge_brush, g_theme.edge_hi_brush, g_theme.dim_brush,
        g_theme.accent_brush, g_theme.accent_hi_brush
    };
    for (int i = 0; i < 8; ++i) {
        if (brushes[i]) {
            DeleteObject(brushes[i]);
        }
    }
    g_theme.ink_brush = g_theme.panel_brush = g_theme.panel_alt_brush =
        g_theme.edge_brush = g_theme.edge_hi_brush = g_theme.dim_brush =
        g_theme.accent_brush = g_theme.accent_hi_brush = NULL;
}

static void ui_rebuild_dpi_assets(int dpi)
{
    ui_release_fonts();
    ui_release_brushes();

    g_theme.ink_brush = CreateSolidBrush(g_theme.ink);
    g_theme.panel_brush = CreateSolidBrush(g_theme.panel);
    g_theme.panel_alt_brush = CreateSolidBrush(g_theme.panel_alt);
    g_theme.edge_brush = CreateSolidBrush(g_theme.edge);
    g_theme.edge_hi_brush = CreateSolidBrush(g_theme.edge_hi);
    g_theme.dim_brush = CreateSolidBrush(g_theme.dim);
    g_theme.accent_brush = CreateSolidBrush(g_theme.accent);
    g_theme.accent_hi_brush = CreateSolidBrush(g_theme.accent_hi);

    g_theme.font[UI_FONT_BODY] = ui_create_font(L"Segoe UI", 9.0, FW_NORMAL, dpi);
    g_theme.font[UI_FONT_BOLD] = ui_create_font(L"Segoe UI", 9.0, FW_SEMIBOLD, dpi);
    g_theme.font[UI_FONT_MONO] = ui_create_font(L"Consolas", 9.0, FW_NORMAL, dpi);
    g_theme.font[UI_FONT_MONO_BOLD] = ui_create_font(L"Consolas", 9.0, FW_SEMIBOLD, dpi);
    g_theme.font[UI_FONT_HEADING] = ui_create_font(L"Segoe UI", 8.0, FW_BOLD, dpi);
    g_theme.font[UI_FONT_CAPTION] = ui_create_font(L"Segoe UI", 8.0, FW_NORMAL, dpi);
    g_theme.font[UI_FONT_VALUE] = ui_create_font(L"Consolas", 21.0, FW_BOLD, dpi);
}

UiTheme *ui_theme(void)
{
    return &g_theme;
}

int ui_px(int v)
{
    return MulDiv(v, g_theme.dpi, 96);
}

void ui_set_dpi(int dpi)
{
    if (dpi <= 0 || dpi == g_theme.dpi) {
        return;
    }
    g_theme.dpi = dpi;
    ui_rebuild_dpi_assets(dpi);
}

void ui_init(void)
{
    if (g_theme.dpi != 0) {
        return;
    }

    g_theme.dpi = 96;
    g_theme.ink = RGB(0x14, 0x17, 0x1C);
    g_theme.panel = RGB(0x1B, 0x21, 0x2B);
    g_theme.panel_alt = RGB(0x24, 0x2C, 0x3A);
    g_theme.row_alt = RGB(0x19, 0x1E, 0x27);
    g_theme.edge = RGB(0x2C, 0x36, 0x48);
    g_theme.edge_hi = RGB(0x3E, 0x4B, 0x61);
    g_theme.text = RGB(0xE8, 0xEC, 0xF4);
    g_theme.dim = RGB(0x8A, 0x94, 0xA8);
    g_theme.accent = RGB(0x3F, 0x9B, 0xFF);
    g_theme.accent_hi = RGB(0x6F, 0xB8, 0xFF);
    g_theme.ok = RGB(0x43, 0xD1, 0x7C);
    g_theme.warn = RGB(0xFF, 0xB4, 0x54);
    g_theme.err = RGB(0xFF, 0x6B, 0x6B);

    ui_rebuild_dpi_assets(g_theme.dpi);

    const struct {
        const wchar_t *name;
        WNDPROC proc;
    } classes[] = {
        { UI_CLASS_PANEL, (WNDPROC)panel_proc },
        { UI_CLASS_BUTTON, (WNDPROC)button_proc },
        { UI_CLASS_SLIDER, (WNDPROC)slider_proc },
        { UI_CLASS_COMBO, (WNDPROC)combo_proc },
        { UI_CLASS_CURVE, (WNDPROC)curve_proc },
        { UI_CLASS_DOT, (WNDPROC)dot_proc },
    };
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = classes[i].proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = classes[i].name;
        RegisterClassW(&wc);
    }
}

void ui_destroy(void)
{
    ui_release_fonts();
    ui_release_brushes();
    g_theme.dpi = 0;
}

/* ---------------------------------------------------------- icon bitmap */

/* ------------------------------------------------------------ paint utils */

static HDC ui_memdc_begin(HWND hwnd, PAINTSTRUCT *ps, RECT *client,
                          HBITMAP *bits, HDC *mem)
{
    *ps = (PAINTSTRUCT){ 0 };
    HDC hdc = BeginPaint(hwnd, ps);
    GetClientRect(hwnd, client);
    *mem = CreateCompatibleDC(hdc);
    *bits = CreateCompatibleBitmap(hdc, client->right, client->bottom);
    SelectObject(*mem, *bits);
    SetBkMode(*mem, TRANSPARENT);
    return hdc;
}

static void ui_memdc_end(HWND hwnd, PAINTSTRUCT *ps, HDC hdc_main,
                         RECT *client, HBITMAP *bits, HDC *mem)
{
    BitBlt(hdc_main, 0, 0, client->right, client->bottom, *mem, 0, 0, SRCCOPY);
    EndPaint(hwnd, ps);
    DeleteObject(*bits);
    DeleteDC(*mem);
}

static void ui_rounded_path(HDC hdc, RECT *r, int radius)
{
    int width = r->right - r->left;
    int height = r->bottom - r->top;
    if (width < 4 || height < 4) {
        return;
    }
    int rad = radius;
    if (rad * 2 > width - 2) {
        rad = (width - 2) / 2;
    }
    if (rad * 2 > height - 2) {
        rad = (height - 2) / 2;
    }
    if (rad < 1) {
        rad = 1;
    }
    RoundRect(hdc, r->left, r->top, r->right - 1, r->bottom - 1, rad * 2, rad * 2);
    CloseFigure(hdc);
}

static void ui_fill_stroke_rounded(HDC hdc, RECT *r, int radius, HBRUSH fill,
                                   HBRUSH edge)
{
    BeginPath(hdc);
    ui_rounded_path(hdc, r, radius);
    EndPath(hdc);

    HGDIOBJ old_brush = SelectObject(hdc, fill ? fill : GetStockObject(NULL_BRUSH));
    HGDIOBJ old_pen = GetStockObject(NULL_PEN);
    FillPath(hdc);
    if (edge) {
        old_pen = SelectObject(hdc, (HGDIOBJ)edge);
        StrokePath(hdc);
    }
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
}

static void ui_draw_text_hdc(HDC hdc, HFONT font, const wchar_t *text,
                             COLORREF color, RECT *r, int fmt)
{
    HGDIOBJ old_font = SelectObject(hdc, font);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    DrawTextW(hdc, text, -1, r, fmt);
    SelectObject(hdc, old_font);
}

/* ------------------------------------------------------- control registry */

static UiRegistryEntry *ui_reg_find(HWND hwnd)
{
    for (int i = 0; i < UI_REGISTRY_MAX; ++i) {
        if (g_registry[i].hwnd == hwnd) {
            return &g_registry[i];
        }
    }
    return NULL;
}

static UiRegistryEntry *ui_reg_new(void)
{
    for (int i = 0; i < UI_REGISTRY_MAX; ++i) {
        if (g_registry[i].hwnd == NULL) {
            return &g_registry[i];
        }
    }
    return NULL;
}

void ui_register_font_role(HWND hwnd, int role)
{
    UiRegistryEntry *entry = ui_reg_find(hwnd);
    if (!entry) {
        entry = ui_reg_new();
        if (!entry) {
            return;
        }
        entry->hwnd = hwnd;
    }
    entry->role = role;
    entry->has_font = true;
}

void ui_register_ctrl(HWND hwnd, int bg, COLORREF color)
{
    UiRegistryEntry *entry = ui_reg_find(hwnd);
    if (!entry) {
        entry = ui_reg_new();
        if (!entry) {
            return;
        }
        entry->hwnd = hwnd;
    }
    entry->bg = bg;
    entry->color = color ? color : g_theme.text;
    entry->has_ctrl = true;
}

void ui_apply_fonts(HWND hwnd)
{
    (void)hwnd;
    for (int i = 0; i < UI_REGISTRY_MAX; ++i) {
        UiRegistryEntry *entry = &g_registry[i];
        if (entry->has_font && IsWindow(entry->hwnd)) {
            SendMessageW(entry->hwnd, WM_SETFONT,
                         (WPARAM)g_theme.font[entry->role], TRUE);
        }
    }
}

HBRUSH ui_handle_ctrl_color(HWND parent, HDC hdc, HWND child)
{
    UiTheme *t = &g_theme;
    UiRegistryEntry *entry = ui_reg_find(child);
    (void)parent;

    wchar_t class_name[16];
    GetClassNameW(child, class_name, 16);
    bool is_edit = lstrcmpW(class_name, WC_EDITW) == 0;

    if (is_edit) {
        SetTextColor(hdc, t->text);
        SetBkColor(hdc, t->ink);
        return t->ink_brush;
    }

    COLORREF color = entry && entry->has_ctrl ? entry->color : t->text;
    COLORREF bg = (entry && entry->has_ctrl && entry->bg == UI_BG_PANEL)
                      ? t->panel
                      : t->ink;
    SetTextColor(hdc, color);
    SetBkColor(hdc, bg);
    return bg == t->panel ? t->panel_brush : t->ink_brush;
}

COLORREF ui_status_color(int state)
{
    UiTheme *t = &g_theme;
    switch (state) {
    case UI_DOT_OK:
        return t->ok;
    case UI_DOT_WARN:
        return t->warn;
    default:
        return t->edge;
    }
}

HWND ui_make_control(HWND parent, const wchar_t *class_name, DWORD extra_style,
                     int x, int y, int w, int h, int id)
{
    /* Only background panels get WS_CLIPSIBLINGS: a panel repaints its whole
     * rectangle, so GDI must clip out the sibling controls that sit above it
     * in the Z-order. Interactive controls must paint unclipped or they would
     * clip themselves away where a panel covers them. */
    DWORD style = WS_CHILD | WS_VISIBLE | extra_style;
    if (lstrcmpW(class_name, UI_CLASS_PANEL) == 0) {
        style |= WS_CLIPSIBLINGS;
    }
    return CreateWindowExW(0, class_name, L"", style, x, y, w, h, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

/* ----------------------------------------------------------------- panels */

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam)
{
    (void)wparam;
    (void)lparam;
    UiTheme *t = &g_theme;

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HBITMAP bits;
        HDC mem;
        HDC hdc = ui_memdc_begin(hwnd, &ps, &client, &bits, &mem);
        FillRect(mem, &client, t->ink_brush);
        ui_fill_stroke_rounded(mem, &client, ui_px(6), t->panel_brush,
                               t->edge_brush);
        ui_memdc_end(hwnd, &ps, hdc, &client, &bits, &mem);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MEASUREITEM: {
        HWND list = ui_panel_list(hwnd);
        if (list && (HWND)wparam == list) {
            return ui_list_handle_measure(list, (MEASUREITEMSTRUCT *)lparam);
        }
        break;
    }
    case WM_DRAWITEM: {
        HWND list = ui_panel_list(hwnd);
        if (list && (HWND)wparam == list) {
            return ui_list_handle_draw(list, (DRAWITEMSTRUCT *)lparam);
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* ---------------------------------------------------------------- buttons */

typedef struct UiButtonData {
    int variant;
    int hot;
    int down;
} UiButtonData;

static void ui_button_notify_click(HWND hwnd)
{
    SendMessageW(GetParent(hwnd), WM_COMMAND,
                 MAKEWPARAM((INT_PTR)GetDlgCtrlID(hwnd), BN_CLICKED),
                 (LPARAM)hwnd);
}

static LRESULT CALLBACK button_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam)
{
    UiButtonData *d = (UiButtonData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    UiTheme *t = &g_theme;
    BOOL enabled = (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;

    switch (msg) {
    case WM_NCCREATE: {
        UiButtonData *data =
            (UiButtonData *)HeapAlloc(GetProcessHeap(), 0, sizeof(UiButtonData));
        ZeroMemory(data, sizeof(UiButtonData));
        data->variant = UI_BTN_SUBTLE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_ENABLE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (d && enabled) {
            d->down = 1;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (d && enabled && !d->hot) {
            d->hot = 1;
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (d && d->hot) {
            d->hot = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        bool click = false;
        if (d) {
            if (d->down) {
                d->down = 0;
                ReleaseCapture();
                RECT r;
                GetClientRect(hwnd, &r);
                POINT p = { 0, 0 };
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                click = enabled && p.x >= 0 && p.y >= 0 && p.x < r.right &&
                        p.y < r.bottom;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            if (click) {
                ui_button_notify_click(hwnd);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (d && enabled && wparam == VK_SPACE) {
            d->down = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_KEYUP:
        if (d && enabled && wparam == VK_SPACE && d->down) {
            d->down = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            ui_button_notify_click(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HBITMAP bits;
        HDC mem;
        HDC hdc = ui_memdc_begin(hwnd, &ps, &client, &bits, &mem);

        int variant = d ? d->variant : UI_BTN_SUBTLE;
        HBRUSH fill;
        HBRUSH border;
        COLORREF text_color;
        HFONT font;

        if (!enabled) {
            fill = t->panel_brush;
            border = t->edge_brush;
            text_color = t->dim;
            font = t->font[UI_FONT_BODY];
        } else if (variant == UI_BTN_PRIMARY) {
            fill = (d && (d->down || d->hot)) ? t->accent_hi_brush
                                              : t->accent_brush;
            border = NULL;
            text_color = t->ink;
            font = t->font[UI_FONT_BOLD];
        } else {
            fill = (d && d->down) ? t->panel_alt_brush : t->panel_brush;
            border = (d && (d->hot || d->down)) ? t->edge_hi_brush : t->edge_brush;
            text_color = t->text;
            font = t->font[UI_FONT_BODY];
        }

        RECT body = client;
        ui_fill_stroke_rounded(mem, &body, ui_px(5), fill, border);

        RECT text_rect = client;
        text_rect.left += ui_px(4);
        text_rect.right -= ui_px(4);
        wchar_t label[64];
        GetWindowTextW(hwnd, label, 64);
        ui_draw_text_hdc(mem, font, label, text_color, &text_rect,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (enabled && GetFocus() == hwnd &&
            (GetWindowLongW(hwnd, GWL_STYLE) & WS_TABSTOP)) {
            RECT ring = client;
            ring.left -= 2;
            ring.top -= 2;
            ring.right += 2;
            ring.bottom += 2;
            ui_fill_stroke_rounded(mem, &ring, ui_px(5) + 1, NULL, t->edge_hi_brush);
        }

        ui_memdc_end(hwnd, &ps, hdc, &client, &bits, &mem);
        return 0;
    }
    case WM_DESTROY: {
        UiButtonData *data = (UiButtonData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (data) {
            HeapFree(GetProcessHeap(), 0, data);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ui_button_set_variant(HWND hwnd, int variant)
{
    UiButtonData *d = (UiButtonData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (d) {
        d->variant = variant;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

/* ---------------------------------------------------------------- sliders */

typedef struct UiSliderData {
    int pos;
    int dragging;
    int key_active;
    int focus;
} UiSliderData;

static void ui_slider_notify(HWND hwnd, int code)
{
    SendMessageW(GetParent(hwnd), WM_HSCROLL, (WPARAM)code, (LPARAM)hwnd);
}

static void ui_slider_pos_from_point(HWND hwnd, UiSliderData *d, int x)
{
    RECT r;
    GetClientRect(hwnd, &r);
    int pad = ui_px(7);
    int track = r.right - r.left - pad * 2;
    if (track <= 0) {
        return;
    }
    int pos = (int)((double)(x - pad) / (double)track * 100.0 + 0.5);
    if (pos < 0) {
        pos = 0;
    }
    if (pos > 100) {
        pos = 100;
    }
    if (pos != d->pos) {
        d->pos = pos;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static LRESULT CALLBACK slider_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam)
{
    UiSliderData *d = (UiSliderData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    UiTheme *t = &g_theme;
    BOOL enabled = (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;

    switch (msg) {
    case WM_NCCREATE: {
        UiSliderData *data =
            (UiSliderData *)HeapAlloc(GetProcessHeap(), 0, sizeof(UiSliderData));
        ZeroMemory(data, sizeof(UiSliderData));
        data->pos = 50;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        if (d) {
            d->focus = (msg == WM_SETFOCUS) ? 1 : 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (d && enabled) {
            d->dragging = 1;
            SetCapture(hwnd);
            ui_slider_pos_from_point(hwnd, d, (int)LOWORD(lparam));
            ui_slider_notify(hwnd, TB_THUMBPOSITION);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (d && d->dragging) {
            ui_slider_pos_from_point(hwnd, d, (int)LOWORD(lparam));
            ui_slider_notify(hwnd, TB_THUMBPOSITION);
        }
        return 0;
    case WM_LBUTTONUP:
        if (d && d->dragging) {
            d->dragging = 0;
            ReleaseCapture();
            ui_slider_notify(hwnd, TB_ENDTRACK);
        }
        return 0;
    case WM_KEYDOWN: {
        if (!d || !enabled) {
            break;
        }
        int next = -1;
        switch (wparam) {
        case VK_LEFT: next = d->pos - 1; break;
        case VK_RIGHT: next = d->pos + 1; break;
        case VK_PRIOR: next = d->pos - 10; break;
        case VK_NEXT: next = d->pos + 10; break;
        case VK_HOME: next = 0; break;
        case VK_END: next = 100; break;
        default: break;
        }
        if (next >= 0) {
            if (next < 0) {
                next = 0;
            }
            if (next > 100) {
                next = 100;
            }
            if (next != d->pos) {
                d->pos = next;
                d->key_active = 1;
                InvalidateRect(hwnd, NULL, FALSE);
                ui_slider_notify(hwnd, TB_THUMBPOSITION);
            }
            return 0;
        }
        break;
    }
    case WM_KEYUP:
        if (d && d->key_active) {
            d->key_active = 0;
            ui_slider_notify(hwnd, TB_ENDTRACK);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HBITMAP bits;
        HDC mem;
        HDC hdc = ui_memdc_begin(hwnd, &ps, &client, &bits, &mem);
        FillRect(mem, &client, t->panel_brush);

        int pad = ui_px(7);
        int track_h = ui_px(4);
        int ty = (client.bottom - track_h) / 2;
        RECT track;
        track.left = pad;
        track.top = ty;
        track.right = client.right - pad;
        track.bottom = ty + track_h;
        ui_fill_stroke_rounded(mem, &track, 2, t->panel_alt_brush, NULL);

        int track_w = track.right - track.left;
        int fill_w = (int)((double)track_w * d->pos / 100.0 + 0.5);
        if (fill_w > 0) {
            RECT fill;
            fill.left = track.left;
            fill.top = track.top;
            fill.right = track.left + fill_w;
            fill.bottom = track.bottom;
            ui_fill_stroke_rounded(mem, &fill, 2,
                                   enabled ? t->accent_brush : t->dim_brush,
                                   NULL);
        }

        int thumb = ui_px(14);
        int cx = pad + fill_w;
        int cy = client.bottom / 2;
        RECT thumb_rect;
        thumb_rect.left = cx - thumb / 2;
        thumb_rect.top = cy - thumb / 2;
        thumb_rect.right = thumb_rect.left + thumb;
        thumb_rect.bottom = thumb_rect.top + thumb;
        HBRUSH thumb_brush =
            CreateSolidBrush(enabled ? (d->dragging ? t->accent_hi : t->accent)
                                     : t->dim);
        HGDIOBJ old_thumb_brush = SelectObject(mem, thumb_brush);
        HGDIOBJ old_thumb_pen = SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, thumb_rect.left, thumb_rect.top, thumb_rect.right + 1,
                thumb_rect.bottom + 1);
        SelectObject(mem, old_thumb_pen);
        SelectObject(mem, old_thumb_brush);
        DeleteObject(thumb_brush);

        if (d->focus && enabled) {
            RECT ring = thumb_rect;
            ring.left -= 3;
            ring.top -= 3;
            ring.right += 3;
            ring.bottom += 3;
            HPEN ring_pen = CreatePen(PS_SOLID, 1, t->accent);
            HGDIOBJ old_pen = SelectObject(mem, ring_pen);
            HGDIOBJ old_brush = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Ellipse(mem, ring.left, ring.top, ring.right + 1, ring.bottom + 1);
            SelectObject(mem, old_brush);
            SelectObject(mem, old_pen);
            DeleteObject(ring_pen);
        }

        ui_memdc_end(hwnd, &ps, hdc, &client, &bits, &mem);
        return 0;
    }
    case WM_DESTROY: {
        UiSliderData *data = (UiSliderData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (data) {
            HeapFree(GetProcessHeap(), 0, data);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int ui_slider_value(HWND hwnd)
{
    UiSliderData *d = (UiSliderData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return d ? d->pos : 0;
}

void ui_slider_set(HWND hwnd, int value)
{
    UiSliderData *d = (UiSliderData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!d) {
        return;
    }
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    d->pos = value;
    InvalidateRect(hwnd, NULL, FALSE);
}

/* ----------------------------------------------------------------- combos */

typedef struct UiComboData {
    wchar_t items[UI_COMBO_ITEM_COUNT][48];
    int count;
    int sel;
    int hot;
} UiComboData;

static void ui_combo_open_menu(UiComboData *d, HWND hwnd)
{
    if (d->count == 0) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    for (int i = 0; i < d->count; ++i) {
        AppendMenuW(menu, MF_STRING | (i == d->sel ? MF_CHECKED : 0), i,
                    d->items[i]);
    }
    RECT rc;
    GetWindowRect(hwnd, &rc);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, rc.left,
                             rc.bottom, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= 0 && cmd < d->count && cmd != d->sel) {
        d->sel = cmd;
        InvalidateRect(hwnd, NULL, FALSE);
        SendMessageW(GetParent(hwnd), WM_COMMAND,
                     MAKEWPARAM((INT_PTR)GetDlgCtrlID(hwnd), CBN_SELCHANGE),
                     (LPARAM)hwnd);
    }
}

static LRESULT CALLBACK combo_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam)
{
    UiComboData *d = (UiComboData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    UiTheme *t = &g_theme;
    BOOL enabled = (GetWindowLongW(hwnd, GWL_STYLE) & WS_VISIBLE) != 0;

    switch (msg) {
    case WM_NCCREATE: {
        UiComboData *data =
            (UiComboData *)HeapAlloc(GetProcessHeap(), 0, sizeof(UiComboData));
        ZeroMemory(data, sizeof(UiComboData));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_ENABLE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (d && enabled) {
            ui_combo_open_menu(d, hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (d && enabled && !d->hot) {
            d->hot = 1;
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (d && d->hot) {
            d->hot = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (d && enabled && d->count > 0) {
            if (wparam == VK_UP) {
                d->sel = (d->sel + d->count - 1) % d->count;
                InvalidateRect(hwnd, NULL, FALSE);
                SendMessageW(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM((INT_PTR)GetDlgCtrlID(hwnd), CBN_SELCHANGE),
                             (LPARAM)hwnd);
                return 0;
            }
            if (wparam == VK_DOWN) {
                d->sel = (d->sel + 1) % d->count;
                InvalidateRect(hwnd, NULL, FALSE);
                SendMessageW(GetParent(hwnd), WM_COMMAND,
                             MAKEWPARAM((INT_PTR)GetDlgCtrlID(hwnd), CBN_SELCHANGE),
                             (LPARAM)hwnd);
                return 0;
            }
            if (wparam == VK_SPACE || wparam == VK_RETURN) {
                ui_combo_open_menu(d, hwnd);
                return 0;
            }
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HBITMAP bits;
        HDC mem;
        HDC hdc = ui_memdc_begin(hwnd, &ps, &client, &bits, &mem);

        HBRUSH fill = (d && d->hot) ? t->panel_alt_brush : t->panel_brush;
        HBRUSH border = (d && d->hot) ? t->edge_hi_brush : t->edge_brush;
        ui_fill_stroke_rounded(mem, &client, ui_px(5), fill, border);

        COLORREF text_color = enabled ? t->text : t->dim;
        if (d && d->count > 0 && d->sel >= 0 && d->sel < d->count) {
            RECT text_rect = client;
            text_rect.left += ui_px(10);
            text_rect.right -= ui_px(22);
            ui_draw_text_hdc(mem, t->font[UI_FONT_BODY], d->items[d->sel],
                             text_color, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        if (enabled) {
            POINT tri[3];
            tri[0].x = client.right - ui_px(18);
            tri[0].y = client.bottom / 2 - ui_px(3);
            tri[1].x = client.right - ui_px(10);
            tri[1].y = client.bottom / 2 - ui_px(3);
            tri[2].x = client.right - ui_px(14);
            tri[2].y = client.bottom / 2 + ui_px(3);
            HGDIOBJ old_brush = SelectObject(mem, t->dim_brush);
            HGDIOBJ old_pen = SelectObject(mem, GetStockObject(NULL_PEN));
            Polygon(mem, tri, 3);
            SelectObject(mem, old_pen);
            SelectObject(mem, old_brush);
        }

        if (enabled && GetFocus() == hwnd &&
            (GetWindowLongW(hwnd, GWL_STYLE) & WS_TABSTOP)) {
            RECT ring = client;
            ring.left -= 2;
            ring.top -= 2;
            ring.right += 2;
            ring.bottom += 2;
            ui_fill_stroke_rounded(mem, &ring, ui_px(6), NULL, t->edge_hi_brush);
        }

        ui_memdc_end(hwnd, &ps, hdc, &client, &bits, &mem);
        return 0;
    }
    case WM_DESTROY: {
        UiComboData *data = (UiComboData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (data) {
            HeapFree(GetProcessHeap(), 0, data);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ui_combo_set_items(HWND hwnd, int count, const wchar_t *const *items)
{
    UiComboData *d = (UiComboData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!d || !items) {
        return;
    }
    if (count > UI_COMBO_ITEM_COUNT) {
        count = UI_COMBO_ITEM_COUNT;
    }
    d->count = count;
    for (int i = 0; i < count; ++i) {
        size_t len = wcslen(items[i]);
        if (len > 47) {
            len = 47;
        }
        memcpy(d->items[i], items[i], len * sizeof(wchar_t));
        d->items[i][len] = L'\0';
    }
    if (d->sel < 0 || d->sel >= d->count) {
        d->sel = 0;
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

void ui_combo_set_selected(HWND hwnd, int index)
{
    UiComboData *d = (UiComboData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (d && index >= 0 && index < d->count) {
        d->sel = index;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

int ui_combo_selected(HWND hwnd)
{
    UiComboData *d = (UiComboData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return d ? d->sel : -1;
}

/* ----------------------------------------------------------- GPU curve */

typedef struct UiCurveData {
    int t_low;
    int d_low;
    int t_high;
    int d_high;
    int t_now;
    int now_ok;
} UiCurveData;

static int ui_curve_value_at(int temp, const UiCurveData *c)
{
    if (c->t_high <= c->t_low) {
        return temp >= c->t_low ? c->d_high : c->d_low;
    }
    if (temp <= c->t_low) {
        return c->d_low;
    }
    if (temp >= c->t_high) {
        return c->d_high;
    }
    return c->d_low + ((temp - c->t_low) * (c->d_high - c->d_low)) /
           (c->t_high - c->t_low);
}

static LRESULT CALLBACK curve_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                   LPARAM lparam)
{
    (void)wparam;
    (void)lparam;
    UiCurveData *d = (UiCurveData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    UiTheme *t = &g_theme;

    switch (msg) {
    case WM_NCCREATE: {
        UiCurveData *data =
            (UiCurveData *)HeapAlloc(GetProcessHeap(), 0, sizeof(UiCurveData));
        ZeroMemory(data, sizeof(UiCurveData));
        data->t_low = 40;
        data->d_low = 25;
        data->t_high = 80;
        data->d_high = 100;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HBITMAP bits;
        HDC mem;
        HDC hdc = ui_memdc_begin(hwnd, &ps, &client, &bits, &mem);
        FillRect(mem, &client, t->panel_brush);

        int pad_l = ui_px(30);
        int pad_r = ui_px(8);
        int pad_t = ui_px(8);
        int pad_b = ui_px(18);
        int plot_w = client.right - pad_l - pad_r;
        int plot_h = client.bottom - pad_t - pad_b;
        if (plot_w < 40 || plot_h < 40) {
            ui_memdc_end(hwnd, &ps, hdc, &client, &bits, &mem);
            return 0;
        }

        int x_max = 90;
        if (d->t_high + 5 > x_max) {
            x_max = d->t_high + 5;
        }
        if (d->t_low + 5 > x_max) {
            x_max = d->t_low + 5;
        }
        if (d->now_ok && d->t_now + 5 > x_max) {
            x_max = d->t_now + 5;
        }

        /* Grid lines + labels. */
        int step = x_max > 90 ? 20 : 10;
        for (int temp = step; temp < x_max; temp += step) {
            int x = pad_l + (int)((double)temp / (double)x_max * plot_w + 0.5);
            HPEN grid_pen = CreatePen(PS_SOLID, 1, t->panel_alt);
            HGDIOBJ old_pen = SelectObject(mem, grid_pen);
            MoveToEx(mem, x, pad_t, NULL);
            LineTo(mem, x, pad_t + plot_h);
            SelectObject(mem, old_pen);
            DeleteObject(grid_pen);
            if (temp % 20 == 0 || x_max <= 90) {
                wchar_t label[8];
                swprintf(label, 8, L"%d", temp);
                RECT lr;
                lr.left = x - ui_px(14);
                lr.top = client.bottom - pad_b + 2;
                lr.right = x + ui_px(14);
                lr.bottom = client.bottom;
                ui_draw_text_hdc(mem, t->font[UI_FONT_CAPTION], label, t->dim,
                                 &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
            }
        }
        const int duty_lines[3] = { 0, 50, 100 };
        for (int i = 0; i < 3; ++i) {
            int duty = duty_lines[i];
            int y = pad_t + (int)((100.0 - duty) / 100.0 * plot_h + 0.5);
            HPEN grid_pen = CreatePen(PS_SOLID, 1, t->panel_alt);
            HGDIOBJ old_pen = SelectObject(mem, grid_pen);
            MoveToEx(mem, pad_l, y, NULL);
            LineTo(mem, pad_l + plot_w, y);
            SelectObject(mem, old_pen);
            DeleteObject(grid_pen);
            wchar_t label[8];
            swprintf(label, 8, L"%d", duty);
            RECT lr;
            lr.left = 2;
            lr.top = y - ui_px(8);
            lr.right = pad_l - ui_px(4);
            lr.bottom = y + ui_px(8);
            ui_draw_text_hdc(mem, t->font[UI_FONT_CAPTION], label, t->dim,
                             &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        /* Curve (two-point interpolation, clamped). */
        double px_per_temp = (double)plot_w / (double)x_max;
        int cx_low = pad_l + (int)(d->t_low * px_per_temp + 0.5);
        int cx_high = pad_l + (int)(d->t_high * px_per_temp + 0.5);
        int cy_low = pad_t + (int)((100.0 - d->d_low) / 100.0 * plot_h + 0.5);
        int cy_high = pad_t + (int)((100.0 - d->d_high) / 100.0 * plot_h + 0.5);

        HPEN glow_pen = CreatePen(PS_SOLID, 5, RGB(0x27, 0x5A, 0x8C));
        HPEN line_pen = CreatePen(PS_SOLID, 2, t->accent);
        HGDIOBJ old_pen = SelectObject(mem, glow_pen);
        if (d->t_high <= d->t_low) {
            MoveToEx(mem, pad_l, cy_low, NULL);
            LineTo(mem, cx_low, cy_low);
            LineTo(mem, cx_low, cy_high);
            LineTo(mem, pad_l + plot_w, cy_high);
        } else {
            MoveToEx(mem, pad_l, cy_low, NULL);
            LineTo(mem, cx_low, cy_low);
            LineTo(mem, cx_high, cy_high);
            LineTo(mem, pad_l + plot_w, cy_high);
        }
        SelectObject(mem, line_pen);
        if (d->t_high <= d->t_low) {
            MoveToEx(mem, pad_l, cy_low, NULL);
            LineTo(mem, cx_low, cy_low);
            LineTo(mem, cx_low, cy_high);
            LineTo(mem, pad_l + plot_w, cy_high);
        } else {
            MoveToEx(mem, pad_l, cy_low, NULL);
            LineTo(mem, cx_low, cy_low);
            LineTo(mem, cx_high, cy_high);
            LineTo(mem, pad_l + plot_w, cy_high);
        }
        SelectObject(mem, old_pen);
        DeleteObject(glow_pen);
        DeleteObject(line_pen);

        /* Current temperature marker. */
        if (d->now_ok) {
            int t_now = d->t_now;
            if (t_now < 0) {
                t_now = 0;
            }
            if (t_now > x_max) {
                t_now = x_max;
            }
            int dot_x = pad_l + (int)(t_now * px_per_temp + 0.5);
            int dot_y = pad_t +
                (int)((100.0 - ui_curve_value_at(d->t_now, d)) / 100.0 * plot_h +
                      0.5);
            RECT dot;
            dot.left = dot_x - 6;
            dot.top = dot_y - 6;
            dot.right = dot_x + 6;
            dot.bottom = dot_y + 6;
            HBRUSH dot_brush = CreateSolidBrush(t->accent_hi);
            HGDIOBJ old_brush = SelectObject(mem, dot_brush);
            HGDIOBJ old_pen2 = SelectObject(mem, GetStockObject(NULL_PEN));
            Ellipse(mem, dot.left, dot.top, dot.right + 1, dot.bottom + 1);
            SelectObject(mem, old_pen2);
            SelectObject(mem, old_brush);
            DeleteObject(dot_brush);

            RECT core;
            core.left = dot_x - 2;
            core.top = dot_y - 2;
            core.right = dot_x + 2;
            core.bottom = dot_y + 2;
            HBRUSH core_brush = CreateSolidBrush(RGB(0xF2, 0xF6, 0xFC));
            old_brush = SelectObject(mem, core_brush);
            Ellipse(mem, core.left, core.top, core.right + 1, core.bottom + 1);
            SelectObject(mem, old_brush);
            DeleteObject(core_brush);
        }

        ui_memdc_end(hwnd, &ps, hdc, &client, &bits, &mem);
        return 0;
    }
    case WM_DESTROY: {
        UiCurveData *data = (UiCurveData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (data) {
            HeapFree(GetProcessHeap(), 0, data);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ui_curve_set_points(HWND hwnd, int temp_low, int duty_low, int temp_high,
                         int duty_high)
{
    UiCurveData *d = (UiCurveData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (d) {
        d->t_low = temp_low;
        d->d_low = duty_low;
        d->t_high = temp_high;
        d->d_high = duty_high;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

void ui_curve_set_now(HWND hwnd, int temperature_c, bool ok)
{
    UiCurveData *d = (UiCurveData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (d) {
        d->t_now = temperature_c;
        d->now_ok = ok ? 1 : 0;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

/* -------------------------------------------------------------- dot */

typedef struct UiDotData {
    int state;
} UiDotData;

static LRESULT CALLBACK dot_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                 LPARAM lparam)
{
    (void)lparam;
    UiDotData *d = (UiDotData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_NCCREATE: {
        UiDotData *data = (UiDotData *)HeapAlloc(GetProcessHeap(), 0,
                                                 sizeof(UiDotData));
        ZeroMemory(data, sizeof(UiDotData));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)data);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT client;
        HDC hdc = BeginPaint(hwnd, &ps);
        int size = client.right < client.bottom ? client.right : client.bottom;
        if (size > ui_px(10)) {
            size = ui_px(10);
        }
        if (size < 4) {
            size = 4;
        }
        int cx = client.right / 2;
        int cy = client.bottom / 2;
        HBRUSH brush = CreateSolidBrush(ui_status_color(d ? d->state : UI_DOT_OFF));
        HGDIOBJ old_brush = SelectObject(hdc, brush);
        HGDIOBJ old_pen = SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, cx - size / 2, cy - size / 2, cx + size / 2 + 1,
                cy + size / 2 + 1);
        SelectObject(hdc, old_pen);
        SelectObject(hdc, old_brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY: {
        UiDotData *data = (UiDotData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (data) {
            HeapFree(GetProcessHeap(), 0, data);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ui_dot_set(HWND hwnd, int state)
{
    UiDotData *d = (UiDotData *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (d && d->state != state) {
        d->state = state;
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

/* ------------------------------------------------------ dark listviews */

typedef struct UiPanelListMap {
    HWND panel;
    HWND list;
} UiPanelListMap;

static UiPanelListMap g_panel_lists[UI_LIST_MAX];
static int g_panel_list_count;

HWND ui_panel_list(HWND panel)
{
    for (int i = 0; i < g_panel_list_count; ++i) {
        if (g_panel_lists[i].panel == panel) {
            return g_panel_lists[i].list;
        }
    }
    return NULL;
}

void ui_panel_attach_list(HWND panel, HWND list)
{
    if (g_panel_list_count >= UI_LIST_MAX) {
        return;
    }
    g_panel_lists[g_panel_list_count].panel = panel;
    g_panel_lists[g_panel_list_count].list = list;
    ++g_panel_list_count;
}

typedef struct UiListState {
    HWND list;
    HWND header;
    unsigned numeric_mask;
    UiListTextFn text_fn;
    void *ctx;
    LRESULT CALLBACK (*old_list_proc)(HWND, UINT, WPARAM, LPARAM);
    LRESULT CALLBACK (*old_header_proc)(HWND, UINT, WPARAM, LPARAM);
} UiListState;

static UiListState g_lists[UI_LIST_MAX];

static UiListState *ui_list_find(HWND list)
{
    for (int i = 0; i < UI_LIST_MAX; ++i) {
        if (g_lists[i].list == list) {
            return &g_lists[i];
        }
    }
    return NULL;
}

static COLORREF ui_list_text_color(const wchar_t *text)
{
    UiTheme *t = &g_theme;
    if (lstrcmpW(text, L"N/A") == 0 || lstrcmpW(text, L"-") == 0 ||
        lstrcmpW(text, L"Unavailable") == 0) {
        return t->dim;
    }
    if (wcsncmp(text, L"Offline", 7) == 0) {
        return t->err;
    }
    if (wcsncmp(text, L"Degraded", 8) == 0) {
        return t->warn;
    }
    if (lstrcmpW(text, L"Online") == 0) {
        return t->ok;
    }
    if (lstrcmpW(text, L"NVIDIA") == 0) {
        return t->accent;
    }
    return t->text;
}

bool ui_list_handle_measure(HWND list, MEASUREITEMSTRUCT *mi)
{
    UiListState *ls = ui_list_find(list);
    if (!ls) {
        return false;
    }
    int row = HIWORD((DWORD)mi->itemID);
    int sub = LOWORD((DWORD)mi->itemID);
    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_WIDTH;
    ListView_GetColumn(list, sub, &col);
    mi->itemWidth = (UINT)(col.cx + ui_px(16));
    mi->itemHeight = (UINT)ui_px(24);
    (void)row;
    return true;
}

bool ui_list_handle_draw(HWND list, DRAWITEMSTRUCT *dis)
{
    UiListState *ls = ui_list_find(list);
    if (!ls || !ls->text_fn) {
        return false;
    }
    UiTheme *t = &g_theme;
    int row = HIWORD((DWORD)dis->itemID);
    int sub = LOWORD((DWORD)dis->itemID);

    COLORREF bg;
    if (dis->itemState & ODS_SELECTED) {
        bg = RGB((GetRValue(t->ink) * 3 + GetRValue(t->accent)) / 4,
                 (GetGValue(t->ink) * 3 + GetGValue(t->accent)) / 4,
                 (GetBValue(t->ink) * 3 + GetBValue(t->accent)) / 4);
    } else {
        bg = (row % 2 == 0) ? t->ink : t->row_alt;
    }
    HBRUSH bg_brush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, bg_brush);
    DeleteObject(bg_brush);

    wchar_t text[160];
    text[0] = L'\0';
    ls->text_fn(ls->ctx, row, sub, text, 160);

    HFONT font = (ls->numeric_mask & (1u << sub)) ? t->font[UI_FONT_MONO]
                                                  : t->font[UI_FONT_BODY];
    HGDIOBJ old_font = SelectObject(dis->hDC, font);
    SetTextColor(dis->hDC, ui_list_text_color(text));
    SetBkMode(dis->hDC, TRANSPARENT);
    RECT r = dis->rcItem;
    r.left += ui_px(8);
    r.right -= ui_px(8);
    DrawTextW(dis->hDC, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, old_font);
    return true;
}

static LRESULT CALLBACK listview_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam)
{
    UiListState *ls = (UiListState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    UiTheme *t = &g_theme;

    if (ls) {
        if (msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLORLISTVIEW) {
            HDC hdc = (HDC)wparam;
            SetBkColor(hdc, t->ink);
            SetTextColor(hdc, t->text);
            return (LRESULT)t->ink_brush;
        }
        if (msg == WM_MEASUREITEM && (HWND)wparam == ls->header) {
            MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lparam;
            int col = (int)mi->itemID;
            LVCOLUMNW coldef;
            ZeroMemory(&coldef, sizeof(coldef));
            wchar_t title[64];
            ZeroMemory(title, sizeof(title));
            coldef.mask = LVCF_TEXT;
            coldef.pszText = title;
            ListView_GetColumn(ls->list, col, &coldef);
            HDC dc = GetDC(NULL);
            HFONT old = (HFONT)SelectObject(dc, t->font[UI_FONT_HEADING]);
            SIZE sz;
            GetTextExtentPoint32W(dc, title, (int)wcslen(title), &sz);
            SelectObject(dc, old);
            ReleaseDC(NULL, dc);
            mi->itemWidth = (UINT)(sz.cx + ui_px(24));
            mi->itemHeight = (UINT)ui_px(26);
            return TRUE;
        }
        if (msg == WM_DRAWITEM && (HWND)wparam == ls->header) {
            DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lparam;
            int col = (int)dis->itemID;
            wchar_t title[64];
            ZeroMemory(title, sizeof(title));
            LVCOLUMNW coldef;
            ZeroMemory(&coldef, sizeof(coldef));
            coldef.mask = LVCF_TEXT;
            coldef.pszText = title;
            ListView_GetColumn(ls->list, col, &coldef);
            FillRect(dis->hDC, &dis->rcItem, t->panel_brush);
            RECT r = dis->rcItem;
            r.left += ui_px(10);
            HFONT old = (HFONT)SelectObject(dis->hDC, t->font[UI_FONT_HEADING]);
            SetTextColor(dis->hDC, t->dim);
            SetBkMode(dis->hDC, TRANSPARENT);
            DrawTextW(dis->hDC, title, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, old);
            return TRUE;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK header_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam)
{
    UiListState *ls = (UiListState *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    UiTheme *t = &g_theme;

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_NCPAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, t->panel_brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    if (ls && ls->old_header_proc) {
        return ls->old_header_proc(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ui_dark_listview(HWND list, unsigned numeric_mask, UiListTextFn text_fn,
                      void *ctx)
{
    if (g_list_count >= UI_LIST_MAX) {
        return;
    }
    UiListState *ls = &g_lists[g_list_count];
    ls->list = list;
    ls->header = FindWindowExW(list, NULL, L"SysHeader32", NULL);
    ls->numeric_mask = numeric_mask;
    ls->text_fn = text_fn;
    ls->ctx = ctx;
    ls->old_list_proc = NULL;
    ls->old_header_proc = NULL;

    ListView_SetExtendedListViewStyle(list, LVS_EX_DOUBLEBUFFER);

    WNDPROC old = (WNDPROC)GetWindowLongPtrW(list, GWLP_WNDPROC);
    SetWindowLongPtrW(list, GWLP_WNDPROC, (LONG_PTR)listview_proc);
    ls->old_list_proc = (LRESULT (CALLBACK *)(HWND, UINT, WPARAM, LPARAM))old;
    SetWindowLongPtrW(list, GWLP_USERDATA, (LONG_PTR)ls);

    if (ls->header) {
        WNDPROC old_header =
            (WNDPROC)GetWindowLongPtrW(ls->header, GWLP_WNDPROC);
        SetWindowLongPtrW(ls->header, GWLP_WNDPROC, (LONG_PTR)header_proc);
        ls->old_header_proc =
            (LRESULT (CALLBACK *)(HWND, UINT, WPARAM, LPARAM))old_header;
        SetWindowLongPtrW(ls->header, GWLP_USERDATA, (LONG_PTR)ls);
    }
    ++g_list_count;
}