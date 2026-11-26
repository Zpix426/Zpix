#include "ui.h"

#include <objidl.h>
#include <gdiplus.h>

using namespace Gdiplus;

namespace ui {

static Color ToColor(COLORREF c, BYTE a = 255) {
    return Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

void RoundRect(HDC dc, RECT rc, int radius, COLORREF fill, COLORREF border, int borderWidth) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    GraphicsPath path;
    {
        int d = radius * 2;
        path.AddArc(rc.left, rc.top, d, d, 180.0f, 90.0f);
        path.AddArc(rc.right - d, rc.top, d, d, 270.0f, 90.0f);
        path.AddArc(rc.right - d, rc.bottom - d, d, d, 0.0f, 90.0f);
        path.AddArc(rc.left, rc.bottom - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }
    if (fill != CLR_INVALID) {
        SolidBrush b(ToColor(fill));
        g.FillPath(&b, &path);
    }
    if (border != CLR_INVALID) {
        Pen pen(ToColor(border), (REAL)(borderWidth > 0 ? borderWidth : 1));
        pen.SetAlignment(PenAlignmentInset);
        g.DrawPath(&pen, &path);
    }
}

void Text(HDC dc, const std::wstring& s, RECT rc, COLORREF color, HFONT font, UINT flags) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), (int)s.size(), &rc, flags);
    SelectObject(dc, old);
}

void TextLine(HDC dc, const std::wstring& s, int x, int y, int w, int h, COLORREF color, HFONT font,
              UINT flags) {
    RECT rc{x, y, x + w, y + h};
    Text(dc, s, rc, color, font, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void Toggle(HDC dc, RECT rc, float t, bool hover) {
    // 轨道
    COLORREF off = hover ? MixColor(TrackOff(), RGB(203, 208, 217), 1.0f) : TrackOff();
    COLORREF track = MixColor(off, Accent(), t);
    RoundRect(dc, rc, (rc.bottom - rc.top) / 2, track, CLR_INVALID);

    // 滑块
    int pad = 2;
    int d = (rc.bottom - rc.top) - pad * 2;
    int travel = (rc.right - rc.left) - d - pad * 2;
    int x = rc.left + pad + (int)(travel * t + 0.5f);
    RECT knob{x, rc.top + pad, x + d, rc.bottom - pad};
    RoundRect(dc, knob, d / 2, RGB(255, 255, 255), CLR_INVALID);
}

void DrawIcon22(HDC dc, HICON icon, int x, int y, int size) {
    if (!icon) return;
    DrawIconEx(dc, x, y, icon, size, size, 0, NULL, DI_NORMAL);
}

}  // namespace ui


// ---------------------------------------------------------------- 自绘圆角下拉弹窗

#include "util.h"
#include <windowsx.h>
#include <algorithm>

namespace ui {
namespace {

struct DropPopupState {
    HWND hwnd = nullptr;
    int scale = 100;
    int width = 180;
    int height = 100;
    std::vector<DropdownItem> items;
    std::vector<RECT> rowRects;
    int hoverIdx = -1;
    int pressedIdx = -1;
    int chosenId = -1;
    bool open = true;
    bool tracking = false;
    HFONT fLabel = nullptr;
    HFONT fLabelBold = nullptr;
    HFONT fSub = nullptr;
};

static int DS(const DropPopupState* st, int v) {
    return st ? MulDiv(v, st->scale, 100) : v;
}

static int HitDropRow(const DropPopupState* st, POINT pt) {
    if (!st) return -1;
    for (size_t i = 0; i < st->items.size() && i < st->rowRects.size(); ++i) {
        if (st->items[i].id < 0) continue;
        if (PtInRect(&st->rowRects[i], pt)) return (int)i;
    }
    return -1;
}

static void DrawDropCheck(HDC dc, RECT rc, COLORREF col, int scale) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    float sw = std::max(1.6f, MulDiv(16, scale, 100) / 9.0f);
    Pen pen(ToColor(col), sw);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    float s = (float)MulDiv(11, scale, 100);
    PointF pts[3] = {
        {cx - s * 0.45f, cy + s * 0.04f},
        {cx - s * 0.12f, cy + s * 0.36f},
        {cx + s * 0.48f, cy - s * 0.32f},
    };
    g.DrawLines(&pen, pts, 3);
}

static void PaintDropPopup(HDC dc, const DropPopupState* st) {
    if (!st) return;
    RECT all{0, 0, st->width, st->height};
    RoundRect(dc, all, DS(st, 10), Bg(), RGB(220, 224, 232), 1);

    int checkW = DS(st, 24);
    int padX = DS(st, 12);

    for (size_t i = 0; i < st->items.size() && i < st->rowRects.size(); ++i) {
        const DropdownItem& it = st->items[i];
        const RECT& r = st->rowRects[i];
        if (it.id < 0) {
            int my = (r.top + r.bottom) / 2;
            RECT sep{r.left + DS(st, 6), my, r.right - DS(st, 6), my + 1};
            HBRUSH hb = CreateSolidBrush(Hairline());
            FillRect(dc, &sep, hb);
            DeleteObject(hb);
            continue;
        }

        bool hov = ((int)i == st->hoverIdx);
        bool prs = ((int)i == st->pressedIdx);
        if (hov || it.checked) {
            COLORREF bg = it.checked
                              ? (hov ? RGB(255, 239, 214) : AccentSoft())
                              : (prs ? RGB(235, 238, 244) : RGB(243, 245, 248));
            RoundRect(dc, r, DS(st, 7), bg, CLR_INVALID);
        }

        COLORREF fg = it.checked ? Accent() : Text();
        HFONT lf = it.checked ? st->fLabelBold : st->fLabel;

        int rightEdge = r.right - checkW;
        if (!it.sub.empty()) {
            RECT subRc{r.left + padX, r.top, rightEdge - DS(st, 2), r.bottom};
            Text(dc, it.sub, subRc, it.checked ? Accent() : SubText(), st->fSub,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        RECT lblRc{r.left + padX, r.top, rightEdge, r.bottom};
        Text(dc, it.label, lblRc, fg, lf, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (it.checked) {
            RECT ckRc{r.right - checkW, r.top, r.right - DS(st, 4), r.bottom};
            DrawDropCheck(dc, ckRc, Accent(), st->scale);
        }
    }
}

static LRESULT CALLBACK DropPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DropPopupState* st = (DropPopupState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1,
                                             rc.bottom > 0 ? rc.bottom : 1);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH wbr = CreateSolidBrush(Bg());
        FillRect(mem, &rc, wbr);
        DeleteObject(wbr);
        PaintDropPopup(mem, st);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && st) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorW(NULL, HitDropRow(st, pt) >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (st) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = HitDropRow(st, pt);
            if (idx != st->hoverIdx) {
                st->hoverIdx = idx;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (!st->tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                st->tracking = true;
            }
            return 0;
        }
        break;
    case WM_MOUSELEAVE:
        if (st) {
            st->tracking = false;
            st->hoverIdx = -1;
            st->pressedIdx = -1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (st) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            st->pressedIdx = HitDropRow(st, pt);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (st) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = HitDropRow(st, pt);
            if (idx >= 0) {
                st->chosenId = st->items[idx].id;
                st->open = false;
            } else {
                st->pressedIdx = -1;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && st) {
            st->open = false;
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && st) {
            st->open = false;
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int ShowDropdownPopup(HWND owner, RECT anchorScreen, const std::vector<DropdownItem>& items,
                      int scale, const RECT* clampScreen) {
    if (items.empty()) return -1;
    if (scale < 100) scale = 100;

    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DropPopupProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"ZpixDropdownPopup";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        clsDone = true;
    }

    DropPopupState st;
    st.scale = scale;
    st.items = items;
    st.fLabel = MakeUiFont(MulDiv(13, scale, 100), false);
    st.fLabelBold = MakeUiFont(MulDiv(13, scale, 100), true);
    st.fSub = MakeUiFont(MulDiv(11, scale, 100), false);

    // 测量最佳宽高
    HDC dc = GetDC(owner);
    int maxRowW = MulDiv(136, scale, 100);
    int anchorW = anchorScreen.right - anchorScreen.left;
    if (anchorW > maxRowW) maxRowW = anchorW;

    int padOuter = DS(&st, 6);
    int rowH = DS(&st, 32);
    int sepH = DS(&st, 9);
    int checkW = DS(&st, 24);
    int padX = DS(&st, 12);

    int curY = padOuter;
    for (const auto& it : items) {
        if (it.id < 0) {
            curY += sepH;
            continue;
        }
        SIZE s1{}, s2{};
        HGDIOBJ of = SelectObject(dc, st.fLabelBold);
        GetTextExtentPoint32W(dc, it.label.c_str(), (int)it.label.size(), &s1);
        if (!it.sub.empty()) {
            SelectObject(dc, st.fSub);
            GetTextExtentPoint32W(dc, it.sub.c_str(), (int)it.sub.size(), &s2);
        }
        SelectObject(dc, of);
        int needW = padOuter * 2 + padX + s1.cx + (s2.cx > 0 ? DS(&st, 14) + s2.cx : 0) + checkW +
                    DS(&st, 4);
        if (needW > maxRowW) maxRowW = needW;
        curY += rowH + DS(&st, 2);
    }
    ReleaseDC(owner, dc);
    curY = curY - DS(&st, 2) + padOuter;

    st.width = maxRowW;
    st.height = curY;

    int ry = padOuter;
    for (const auto& it : items) {
        int h = (it.id < 0) ? sepH : rowH;
        st.rowRects.push_back(RECT{padOuter, ry, st.width - padOuter, ry + h});
        ry += h + (it.id < 0 ? 0 : DS(&st, 2));
    }

    POINT probe{(anchorScreen.left + anchorScreen.right) / 2, anchorScreen.bottom};
    RECT work = MonitorWorkRect(probe);

    int x = anchorScreen.left;
    int y = anchorScreen.bottom + DS(&st, 5);

    // 若右侧超出父窗口卡片右边界，自动改为与触发按钮右对齐，确保绝不超出卡片！
    if (clampScreen && clampScreen->right > clampScreen->left) {
        int maxRight = clampScreen->right - DS(&st, 12);
        int minLeft = clampScreen->left + DS(&st, 12);
        if (x + st.width > maxRight) x = anchorScreen.right - st.width;
        if (x + st.width > maxRight) x = maxRight - st.width;
        if (x < minLeft) x = minLeft;
    }
    if (x + st.width > work.right - DS(&st, 6)) x = work.right - st.width - DS(&st, 6);
    if (x < work.left + DS(&st, 6)) x = work.left + DS(&st, 6);
    if (y + st.height > work.bottom - DS(&st, 8)) y = anchorScreen.top - st.height - DS(&st, 5);
    if (y < work.top + DS(&st, 6)) y = work.top + DS(&st, 6);

    HWND pop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ZpixDropdownPopup", L"",
                               WS_POPUP, x, y, st.width, st.height, owner, NULL,
                               GetModuleHandleW(NULL), &st);
    if (!pop) {
        DeleteObject(st.fLabel);
        DeleteObject(st.fLabelBold);
        DeleteObject(st.fSub);
        return -1;
    }
    st.hwnd = pop;

    typedef HRESULT(WINAPI * PFN_Dwm)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        auto pfn = (PFN_Dwm)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (pfn) {
            int pref = 2;  // DWMWCP_ROUND
            pfn(pop, 33, &pref, sizeof(pref));
        }
    }

    ShowWindow(pop, SW_SHOW);
    UpdateWindow(pop);
    SetForegroundWindow(pop);
    SetFocus(pop);

    MSG msg;
    while (st.open && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage((int)msg.wParam);
            break;
        }
        if ((msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
             msg.message == WM_MBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN ||
             msg.message == WM_NCRBUTTONDOWN || msg.message == WM_MOUSEWHEEL) &&
            msg.hwnd != pop) {
            st.open = false;
            // 把点击转发给原本的窗口，但吞掉落在触发按钮自身的重复关闭点击
            POINT cpt = msg.pt;
            if (!PtInRect(&anchorScreen, cpt)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (IsWindow(pop)) DestroyWindow(pop);
    if (owner && IsWindow(owner)) SetForegroundWindow(owner);
    DeleteObject(st.fLabel);
    DeleteObject(st.fLabelBold);
    DeleteObject(st.fSub);
    return st.chosenId;
}

}  // namespace ui
