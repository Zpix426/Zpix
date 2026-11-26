#include "annotate.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>

RECT NormalizePointRect(POINT a, POINT b) {
    RECT r;
    r.left = a.x < b.x ? a.x : b.x;
    r.top = a.y < b.y ? a.y : b.y;
    r.right = a.x > b.x ? a.x : b.x;
    r.bottom = a.y > b.y ? a.y : b.y;
    return r;
}

void ApplyMosaic(Bitmap32& bm, RECT local, int block) {
    if (bm.empty()) return;
    if (block < 2) block = 2;
    int x0 = local.left < 0 ? 0 : local.left;
    int y0 = local.top < 0 ? 0 : local.top;
    int x1 = local.right > bm.w ? bm.w : local.right;
    int y1 = local.bottom > bm.h ? bm.h : local.bottom;
    if (x1 - x0 < 2 || y1 - y0 < 2) return;

    for (int by = y0; by < y1; by += block) {
        for (int bx = x0; bx < x1; bx += block) {
            int ex = bx + block > x1 ? x1 : bx + block;
            int ey = by + block > y1 ? y1 : by + block;

            uint64_t r = 0, g = 0, b = 0;
            int n = 0;
            for (int y = by; y < ey; ++y) {
                const uint32_t* row = bm.row(y);
                for (int x = bx; x < ex; ++x) {
                    uint32_t c = row[x];
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                    ++n;
                }
            }
            if (!n) continue;
            uint32_t avg = 0xFF000000u | ((uint32_t)(r / n) << 16) | ((uint32_t)(g / n) << 8) |
                           (uint32_t)(b / n);
            for (int y = by; y < ey; ++y) {
                uint32_t* row = bm.row(y);
                for (int x = bx; x < ex; ++x) row[x] = avg;
            }
        }
    }
}

Bitmap32 MakeMosaicPatch(const Bitmap32& shot, POINT a, POINT b, int block) {
    RECT vs = GetVirtualScreenRect();
    RECT r = NormalizePointRect(a, b);
    Bitmap32 out;
    int x0 = r.left - vs.left, y0 = r.top - vs.top;
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return out;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > shot.w) w = shot.w - x0;
    if (y0 + h > shot.h) h = shot.h - y0;
    if (w <= 0 || h <= 0) return out;

    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h);
    for (int y = 0; y < h; ++y)
        memcpy(out.row(y), shot.row(y0 + y) + x0, (size_t)w * 4);

    RECT local{0, 0, w, h};
    ApplyMosaic(out, local, block);
    return out;
}

static Gdiplus::Color ToGdipColor(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void DrawArrow(HDC dc, POINT a, POINT b, int width, COLORREF color) {
    float dx = (float)(b.x - a.x);
    float dy = (float)(b.y - a.y);
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 2.0f) return;

    float ux = dx / len;
    float uy = dy / len;
    float px = -uy;
    float py = ux;

    float w = (float)(width < 1 ? 1 : width);
    float headLen = std::min(len * 0.55f, 12.0f + w * 2.8f);
    float headHalf = headLen * 0.46f;
    float neckLen = headLen * 0.78f;
    float neckHalf = w * 0.62f;
    float tailHalf = std::max(0.9f, w * 0.24f);

    float wx = (float)b.x - ux * headLen;
    float wy = (float)b.y - uy * headLen;
    float nx = (float)b.x - ux * neckLen;
    float ny = (float)b.y - uy * neckLen;

    Gdiplus::PointF pts[7] = {
        {(float)a.x + px * tailHalf, (float)a.y + py * tailHalf},
        {nx + px * neckHalf, ny + py * neckHalf},
        {wx + px * headHalf, wy + py * headHalf},
        {(float)b.x, (float)b.y},
        {wx - px * headHalf, wy - py * headHalf},
        {nx - px * neckHalf, ny - py * neckHalf},
        {(float)a.x - px * tailHalf, (float)a.y - py * tailHalf},
    };

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::SolidBrush brush(ToGdipColor(color));
    g.FillPolygon(&brush, pts, 7);
}

void DrawAnnotations(HDC dc, const std::vector<Ann>& items, POINT offset) {
    for (const auto& it : items) {
        switch (it.type) {
        case AnnType::Rect: {
            RECT r = NormalizePointRect(it.a, it.b);
            int x = r.left + offset.x;
            int y = r.top + offset.y;
            int rw = r.right - r.left;
            int rh = r.bottom - r.top;
            if (rw > 0 && rh > 0) {
                Gdiplus::Graphics g(dc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                Gdiplus::Pen pen(ToGdipColor(it.color), (Gdiplus::REAL)(it.width > 0 ? it.width : 2));
                pen.SetLineJoin(Gdiplus::LineJoinRound);
                g.DrawRectangle(&pen, (Gdiplus::REAL)x, (Gdiplus::REAL)y, (Gdiplus::REAL)rw,
                                (Gdiplus::REAL)rh);
            }
            break;
        }
        case AnnType::Arrow:
            DrawArrow(dc, POINT{it.a.x + offset.x, it.a.y + offset.y},
                      POINT{it.b.x + offset.x, it.b.y + offset.y}, it.width, it.color);
            break;
        case AnnType::Text: {
            int fsz = it.fontSize > 8 ? it.fontSize : 22;
            HFONT font = CreateFontW(-fsz, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            HGDIOBJ of = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            RECT r{it.a.x + offset.x, it.a.y + offset.y, it.a.x + offset.x + 1200,
                   it.a.y + offset.y + 600};
            // 按文字亮度自适应浅/深描边阴影，保证白/黄/黑/彩色在任意背景上都清晰可读
            int lum = (GetRValue(it.color) * 299 + GetGValue(it.color) * 587 +
                       GetBValue(it.color) * 114) /
                      1000;
            COLORREF shadowCol = (lum < 60) ? RGB(240, 240, 245) : RGB(20, 20, 24);
            SetTextColor(dc, shadowCol);
            RECT sh = r;
            sh.left += 1;
            sh.top += 1;
            DrawTextW(dc, it.text.c_str(), -1, &sh,
                      DT_LEFT | DT_TOP | DT_NOCLIP | DT_WORDBREAK | DT_NOPREFIX);
            SetTextColor(dc, it.color);
            DrawTextW(dc, it.text.c_str(), -1, &r,
                      DT_LEFT | DT_TOP | DT_NOCLIP | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, of);
            DeleteObject(font);
            break;
        }
        case AnnType::Mosaic: {
            RECT r = NormalizePointRect(it.a, it.b);
            if (it.cached && !it.patch.empty()) {
                HBITMAP bmp = nullptr;
                HGDIOBJ old = nullptr;
                HDC src = CreateBitmapDC(it.patch, bmp, old);
                BitBlt(dc, r.left + offset.x, r.top + offset.y, it.patch.w, it.patch.h, src, 0,
                       0, SRCCOPY);
                SelectObject(src, old);
                DeleteDC(src);
                DeleteObject(bmp);
            } else {
                OffsetRect(&r, offset.x, offset.y);
                HBRUSH br = CreateSolidBrush(RGB(120, 120, 120));
                FillRect(dc, &r, br);
                DeleteObject(br);
            }
            break;
        }
        }
    }
}

Bitmap32 RenderSelectionFromCrop(const Bitmap32& baseCrop, RECT sel, const std::vector<Ann>& items) {
    Bitmap32 out = baseCrop;
    if (out.empty()) return out;

    // 马赛克先做（作用在底图上），其它标注后画
    for (const auto& it : items) {
        if (it.type != AnnType::Mosaic) continue;
        RECT r = NormalizePointRect(it.a, it.b);
        RECT local{r.left - sel.left, r.top - sel.top, r.right - sel.left, r.bottom - sel.top};
        ApplyMosaic(out, local, it.block);
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = out.w;
    bi.bmiHeader.biHeight = -out.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib && bits) {
        memcpy(bits, out.px.data(), out.px.size() * 4);
        HGDIOBJ old = SelectObject(dc, dib);
        DrawAnnotations(dc, items, POINT{-sel.left, -sel.top});
        GdiFlush();
        memcpy(out.px.data(), bits, out.px.size() * 4);
        SelectObject(dc, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(NULL, screen);

    // 保 alpha 不透明
    for (auto& p : out.px) p |= 0xFF000000u;
    return out;
}

Bitmap32 RenderSelection(const Bitmap32& shot, RECT sel, const std::vector<Ann>& items) {
    return RenderSelectionFromCrop(CropBitmap(shot, sel), sel, items);
}