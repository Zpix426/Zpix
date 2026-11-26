#include "pin.h"
#include "actions.h"
#include "config.h"
#include "util.h"

static const wchar_t* kPinClass = L"ZpixPin";
static const COLORREF kPinBorder = RGB(58, 130, 246);

struct PinWindow {
    HWND hwnd = nullptr;
    Bitmap32 bm;
    double scale = 1.0;
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldObj = nullptr;
    bool dragging = false;
    POINT dragOrigin{};
    RECT dragRect{};
    BYTE alpha = 255;
};

static PinWindow* FindPin(HWND hwnd) {
    return (PinWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static void PinRedraw(PinWindow* p) {
    if (!p || !p->hwnd) return;
    SetWindowPos(p->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(p->hwnd, NULL, FALSE);
}

static void PinApplySize(PinWindow* p) {
    if (!p || !p->hwnd) return;
    int w = (int)(p->bm.w * p->scale + 0.5);
    int h = (int)(p->bm.h * p->scale + 0.5);
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    RECT rc;
    GetWindowRect(p->hwnd, &rc);
    SetWindowPos(p->hwnd, HWND_TOPMOST, rc.left, rc.top, w, h, SWP_NOACTIVATE);
}

static void PinZoomAt(PinWindow* p, int newScalePercent, POINT screenPt) {
    if (!p) return;
    RECT rc;
    GetWindowRect(p->hwnd, &rc);
    double oldScale = p->scale;
    double ns = newScalePercent / 100.0;
    if (ns < 0.1) ns = 0.1;
    if (ns > 8.0) ns = 8.0;
    if (ns == oldScale) return;

    // 保持鼠标下的像素不动
    double lx = (screenPt.x - rc.left) / oldScale;
    double ly = (screenPt.y - rc.top) / oldScale;
    int nw = (int)(p->bm.w * ns + 0.5);
    int nh = (int)(p->bm.h * ns + 0.5);
    int nx = (int)(screenPt.x - lx * ns);
    int ny = (int)(screenPt.y - ly * ns);

    p->scale = ns;
    SetWindowPos(p->hwnd, HWND_TOPMOST, nx, ny, nw, nh, SWP_NOACTIVATE);
    InvalidateRect(p->hwnd, NULL, FALSE);
}

static LRESULT CALLBACK PinProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PinWindow* p = FindPin(hwnd);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        if (p && p->mem) {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, 0, 0, client.right, client.bottom, p->mem, 0, 0, p->bm.w, p->bm.h,
                       SRCCOPY);
            HPEN pen = CreatePen(PS_SOLID, 1, kPinBorder);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, client.right, client.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!p) break;
        p->dragging = true;
        SetCapture(hwnd);
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &pt);
        p->dragOrigin = pt;
        GetWindowRect(hwnd, &p->dragRect);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!p || !p->dragging) break;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &pt);
        int dx = pt.x - p->dragOrigin.x;
        int dy = pt.y - p->dragOrigin.y;
        SetWindowPos(hwnd, HWND_TOPMOST, p->dragRect.left + dx, p->dragRect.top + dy, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (p) p->dragging = false;
        ReleaseCapture();
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!p) break;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int curPercent = (int)(p->scale * 100 + 0.5);
        int step = curPercent < 100 ? 10 : (curPercent < 300 ? 20 : 50);
        int next = curPercent + (delta > 0 ? step : -step);
        // WM_MOUSEWHEEL 的 lParam 本身就是屏幕坐标，不能再 ClientToScreen
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        PinZoomAt(p, next, pt);
        return 0;
    }

    case WM_RBUTTONUP: {
        if (!p) break;
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"复制图片\tCtrl+C");
        AppendMenuW(menu, MF_STRING, 2, L"另存为 PNG\tCtrl+S");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, 3, L"缩放 100%");
        HMENU alphaMenu = CreatePopupMenu();
        AppendMenuW(alphaMenu, MF_STRING, 10, L"100%");
        AppendMenuW(alphaMenu, MF_STRING, 11, L"85%");
        AppendMenuW(alphaMenu, MF_STRING, 12, L"70%");
        AppendMenuW(alphaMenu, MF_STRING, 13, L"50%");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)alphaMenu, L"不透明度");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, 20, L"关闭\tEsc");

        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd,
                                 NULL);
        DestroyMenu(menu);

        if (cmd == 1) {
            CopyToClipboard(hwnd, p->bm);
            ShowToast(L"已复制到剪贴板");
        } else if (cmd == 2) {
            std::wstring dateDir;
            std::wstring path = BuildDailyShotPath(g_cfg.saveDir, &dateDir);
            if (SavePngFile(p->bm, path)) {
                ShowToast(L"已保存：" + path);
                if (g_cfg.openFolderAfterSave) OpenFolderAndSelectFile(path, dateDir);
            } else {
                ShowToast(L"保存失败，请检查保存目录权限");
            }
        } else if (cmd == 3) {
            p->scale = 1.0;
            PinApplySize(p);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (cmd >= 10 && cmd <= 13) {
            BYTE a = cmd == 10 ? 255 : (cmd == 11 ? 217 : (cmd == 12 ? 179 : 128));
            p->alpha = a;
            SetWindowLongW(hwnd, GWL_EXSTYLE, GetWindowLongW(hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
            SetLayeredWindowAttributes(hwnd, 0, a, LWA_ALPHA);
        } else if (cmd == 20) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        ZLog(L"[pin] WM_KEYDOWN vk=%d", (int)wp);
        if (!p) break;
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            CopyToClipboard(hwnd, p->bm);
            ShowToast(L"已复制到剪贴板");
            return 0;
        }
        if (wp == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            std::wstring dateDir;
            std::wstring path = BuildDailyShotPath(g_cfg.saveDir, &dateDir);
            if (SavePngFile(p->bm, path)) {
                ShowToast(L"已保存：" + path);
                if (g_cfg.openFolderAfterSave) OpenFolderAndSelectFile(path, dateDir);
            } else {
                ShowToast(L"保存失败，请检查保存目录权限");
            }
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
        ZLog(L"[pin] WM_CLOSE msg");
        break;
    case WM_SYSCOMMAND:
        ZLog(L"[pin] WM_SYSCOMMAND cmd=%d", (int)(wp & 0xFFF0));
        break;
    case WM_MOUSEACTIVATE:
        ZLog(L"[pin] WM_MOUSEACTIVATE");
        break;
    case WM_TIMER:
        ZLog(L"[pin] timer hwnd=%p IsWindow=%d visible=%d", (void*)hwnd, (int)IsWindow(hwnd), (int)IsWindowVisible(hwnd));
        KillTimer(hwnd, wp);
        break;

    case WM_NCDESTROY:
        ZLog(L"[pin] WM_NCDESTROY hwnd=%p", (void*)hwnd);
        break;

    case WM_SHOWWINDOW:
        ZLog(L"[pin] WM_SHOWWINDOW hwnd=%p show=%d", (void*)hwnd, (int)wp);
        break;

    case WM_DESTROY: {
        ZLog(L"[pin] destroy hwnd=%p", (void*)hwnd);
        if (p) {
            if (p->mem) {
                SelectObject(p->mem, p->oldObj);
                DeleteDC(p->mem);
            }
            if (p->dib) DeleteObject(p->dib);
            delete p;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CreatePinFromBitmap(const Bitmap32& bm, POINT ptTopLeft) {
    ZLog(L"[pin] enter %dx%d at %ld,%ld", bm.w, bm.h, ptTopLeft.x, ptTopLeft.y);
    if (bm.empty()) return;

    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PinProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kPinClass;
    RegisterClassExW(&wc);

    PinWindow* p = new PinWindow();
    p->bm = bm;
    p->scale = 1.0;

    // 大图自动缩到"鼠标所在那块屏"的 70% 以内（不要按整个虚拟桌面算，
    // 双屏时虚拟桌面宽一倍，图可能一半挂在另一块屏上）
    POINT cur{};
    GetCursorPos(&cur);
    RECT work = MonitorWorkRect(cur);
    int maxW = (int)((work.right - work.left) * 0.7);
    int maxH = (int)((work.bottom - work.top) * 0.7);
    if (bm.w > maxW || bm.h > maxH) {
        double s1 = maxW / (double)bm.w;
        double s2 = maxH / (double)bm.h;
        p->scale = s1 < s2 ? s1 : s2;
    }

    int w = (int)(bm.w * p->scale + 0.5);
    int h = (int)(bm.h * p->scale + 0.5);

    p->mem = CreateBitmapDC(bm, p->dib, p->oldObj);

    p->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPinClass, L"Zpix Pin",
                              WS_POPUP, ptTopLeft.x, ptTopLeft.y, w, h, NULL, NULL, inst, p);
    if (!p->hwnd) {
        ZLog(L"[pin] CreateWindow FAILED err=%lu", GetLastError());
        delete p;
        return;
    }
    ZLog(L"[pin] hwnd=%p size=%dx%d scale=%.2f", (void*)p->hwnd, w, h, p->scale);
    ZLog(L"[pin] pre-show IsWindow=%d", (int)IsWindow(p->hwnd));
    ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
    ZLog(L"[pin] post-show IsWindow=%d visible=%d", (int)IsWindow(p->hwnd), (int)IsWindowVisible(p->hwnd));
    SetWindowPos(p->hwnd, HWND_TOPMOST, ptTopLeft.x, ptTopLeft.y, w, h,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);
    UpdateWindow(p->hwnd);
    PinRedraw(p);
}