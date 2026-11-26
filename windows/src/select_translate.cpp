#include "select_translate.h"
#include "actions.h"
#include "config.h"
#include "overlay.h"
#include "translate.h"
#include "ui.h"
#include "util.h"

#include <objidl.h>
#include <gdiplus.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------- 全局钩子状态

HHOOK g_mouseHook = nullptr;
HWND g_hostWnd = nullptr;
HWND g_barWnd = nullptr;

std::mutex g_gestureMu;
POINT g_pendingDownPt{};
POINT g_pendingUpPt{};

bool g_downValid = false;
POINT g_downPt{};
DWORD g_downTick = 0;

POINT g_lastUpPt{};
DWORD g_lastUpTick = 0;
bool g_lastWasDrag = false;

std::atomic<bool> g_capturing{false};

struct SelectReadyPayload {
    std::wstring text;
    POINT anchor{};
};

// ---------------------------------------------------------------- 悬浮工具栏状态

struct SelectBarState {
    HWND hwnd = nullptr;
    int scale = 100;
    int width = 88;
    int height = 40;
    std::wstring text;
    POINT anchor{};
    RECT rcCopy{};
    RECT rcTrans{};
    int hover = 0;    // 0=无 1=复制 2=翻译
    int pressed = 0;
    bool tracking = false;
};

SelectBarState* g_bar = nullptr;
const UINT_PTR kTimerAutoHide = 11;

int BS(int v) { return g_bar ? MulDiv(v, g_bar->scale, 100) : v; }

static void EnableRoundedBar(HWND hwnd) {
    typedef HRESULT(WINAPI * PFN)(HWND, DWORD, LPCVOID, DWORD);
    static PFN pfn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        if (m) pfn = (PFN)GetProcAddress(m, "DwmSetWindowAttribute");
    }
    if (!pfn) return;
    int pref = 2;  // DWMWCP_ROUND
    pfn(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
}

// ---------------------------------------------------------------- 矢量图标绘制（GDI+ 抗锯齿）

static Gdiplus::Color ToCol(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h,
                           float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// 精致双页重叠「复制」矢量图标（Lucide copy 风格）
static void DrawCopyVectorIcon(HDC dc, RECT rc, COLORREF color, COLORREF fillBg) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    float s = (float)BS(16);
    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    float x0 = cx - s * 0.5f;
    float y0 = cy - s * 0.5f;
    float sw = std::max(1.4f, BS(16) / 10.5f);
    float r = s * 0.14f;

    Gdiplus::Pen pen(ToCol(color), sw);
    Gdiplus::SolidBrush bgBrush(ToCol(fillBg));

    // 后方页面
    Gdiplus::GraphicsPath back;
    AddRoundedRect(back, x0 + s * 0.28f, y0 + s * 0.06f, s * 0.64f, s * 0.68f, r);
    g.DrawPath(&pen, &back);

    // 前方页面（先填底色遮住后页交叉线，再描边）
    Gdiplus::GraphicsPath front;
    AddRoundedRect(front, x0 + s * 0.08f, y0 + s * 0.26f, s * 0.64f, s * 0.68f, r);
    g.FillPath(&bgBrush, &front);
    g.DrawPath(&pen, &front);
}

// 精致「文 / A」翻译矢量图标（参考划词工具栏中/A样式）
static void DrawTranslateVectorIcon(HDC dc, RECT rc, COLORREF color) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    float s = (float)BS(17);
    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    float x0 = cx - s * 0.5f;
    float y0 = cy - s * 0.5f;
    float sw = std::max(1.45f, BS(16) / 10.5f);

    Gdiplus::Pen pen(ToCol(color), sw);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    // 左上「文」字矢量笔画
    // 点
    g.DrawLine(&pen, x0 + s * 0.28f, y0 + s * 0.06f, x0 + s * 0.28f, y0 + s * 0.16f);
    // 横
    g.DrawLine(&pen, x0 + s * 0.06f, y0 + s * 0.21f, x0 + s * 0.52f, y0 + s * 0.21f);
    // 撇（弧线感）
    g.DrawBezier(&pen, x0 + s * 0.42f, y0 + s * 0.22f, x0 + s * 0.36f, y0 + s * 0.44f,
                 x0 + s * 0.20f, y0 + s * 0.56f, x0 + s * 0.08f, y0 + s * 0.62f);
    // 捺
    g.DrawBezier(&pen, x0 + s * 0.18f, y0 + s * 0.28f, x0 + s * 0.26f, y0 + s * 0.44f,
                 x0 + s * 0.38f, y0 + s * 0.52f, x0 + s * 0.48f, y0 + s * 0.58f);

    // 右下「A」字矢量笔画
    g.DrawLine(&pen, x0 + s * 0.50f, y0 + s * 0.92f, x0 + s * 0.72f, y0 + s * 0.36f);
    g.DrawLine(&pen, x0 + s * 0.72f, y0 + s * 0.36f, x0 + s * 0.94f, y0 + s * 0.92f);
    g.DrawLine(&pen, x0 + s * 0.58f, y0 + s * 0.75f, x0 + s * 0.86f, y0 + s * 0.75f);

    // 右上极简转折箭头装饰（点缀互译感）
    Gdiplus::Pen thin(ToCol(color, 190), std::max(1.1f, sw * 0.78f));
    thin.SetStartCap(Gdiplus::LineCapRound);
    thin.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&thin, x0 + s * 0.66f, y0 + s * 0.14f, x0 + s * 0.84f, y0 + s * 0.14f);
    g.DrawLine(&thin, x0 + s * 0.84f, y0 + s * 0.14f, x0 + s * 0.84f, y0 + s * 0.26f);
}

// ---------------------------------------------------------------- 悬浮工具栏动作

static void TriggerSelectTranslate(const std::wstring& text, POINT anchor) {
    if (text.empty()) return;
    SetResultAnchor(RECT{anchor.x - 16, anchor.y - 16, anchor.x + 16, anchor.y + 16});

    HWND result = ShowResultWindow(text, true);
    uint64_t rid = CurrentResultReqId();
    std::thread([text, result, rid]() {
        std::vector<TrResult> res = TranslateSelectAll(text);
        TranslatePayload* tp = new TranslatePayload();
        tp->original = text;
        tp->results = std::move(res);
        tp->target = result;
        tp->reqId = rid;
        if (g_msgWnd && IsWindow(g_msgWnd))
            PostMessageW(g_msgWnd, WM_ZPIX_TRANSLATE_DONE, 0, (LPARAM)tp);
        else
            delete tp;
    }).detach();
}

static int HitBarAt(POINT pt) {
    if (!g_bar) return 0;
    if (PtInRect(&g_bar->rcCopy, pt)) return 1;
    if (PtInRect(&g_bar->rcTrans, pt)) return 2;
    return 0;
}

static void PaintSelectBar(HDC dc) {
    if (!g_bar) return;
    RECT all{0, 0, g_bar->width, g_bar->height};
    ui::RoundRect(dc, all, BS(8), RGB(255, 255, 255), RGB(222, 226, 233));

    // 复制按钮
    {
        bool hov = (g_bar->hover == 1);
        bool prs = (g_bar->pressed == 1);
        COLORREF bg = prs ? RGB(236, 239, 244) : (hov ? RGB(244, 245, 248) : RGB(255, 255, 255));
        if (hov || prs) ui::RoundRect(dc, g_bar->rcCopy, BS(7), bg, CLR_INVALID);
        COLORREF ic = hov ? ui::Accent() : RGB(56, 60, 68);
        DrawCopyVectorIcon(dc, g_bar->rcCopy, ic, bg);
    }

    // 中间细分隔线
    {
        int mx = g_bar->width / 2;
        RECT sep{mx, BS(10), mx + 1, g_bar->height - BS(10)};
        HBRUSH hb = CreateSolidBrush(RGB(234, 236, 241));
        FillRect(dc, &sep, hb);
        DeleteObject(hb);
    }

    // 翻译按钮
    {
        bool hov = (g_bar->hover == 2);
        bool prs = (g_bar->pressed == 2);
        COLORREF bg = prs ? RGB(255, 238, 214) : (hov ? ui::AccentSoft() : RGB(255, 255, 255));
        if (hov || prs) ui::RoundRect(dc, g_bar->rcTrans, BS(7), bg, CLR_INVALID);
        COLORREF ic = hov ? ui::Accent() : RGB(56, 60, 68);
        DrawTranslateVectorIcon(dc, g_bar->rcTrans, ic);
    }
}

static LRESULT CALLBACK SelectBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // 点击复制/翻译时不抢目标软件（Chrome/微信等）的窗口焦点
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1,
                                             rc.bottom > 0 ? rc.bottom : 1);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        PaintSelectBar(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETCURSOR: {
        if (LOWORD(lp) != HTCLIENT) break;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        SetCursor(LoadCursorW(NULL, HitBarAt(pt) != 0 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }

    case WM_MOUSEMOVE: {
        if (!g_bar) break;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int h = HitBarAt(pt);
        if (h != g_bar->hover) {
            g_bar->hover = h;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        // 鼠标停在工具栏上时暂缓自动隐藏
        KillTimer(hwnd, kTimerAutoHide);
        if (!g_bar->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_bar->tracking = true;
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_bar) {
            g_bar->tracking = false;
            g_bar->hover = 0;
            g_bar->pressed = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            SetTimer(hwnd, kTimerAutoHide, 4500, NULL);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!g_bar) break;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_bar->pressed = HitBarAt(pt);
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!g_bar) break;
        ReleaseCapture();
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = HitBarAt(pt);
        int prs = g_bar->pressed;
        g_bar->pressed = 0;
        if (hit != 0 && hit == prs) {
            std::wstring text = g_bar->text;
            POINT anchor = g_bar->anchor;
            HideSelectBar();
            if (hit == 1) {
                CopyTextToClipboard(g_msgWnd, text);
                ShowToast(L"已复制选中文字");
            } else if (hit == 2) {
                TriggerSelectTranslate(text, anchor);
            }
            return 0;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wp == kTimerAutoHide) {
            HideSelectBar();
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hwnd, kTimerAutoHide);
        if (g_bar) {
            delete g_bar;
            g_bar = nullptr;
        }
        g_barWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnsureSelectBarWindow() {
    if (g_barWnd && IsWindow(g_barWnd)) return;
    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SelectBarProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"ZpixSelectBar";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        clsDone = true;
    }
    g_bar = new SelectBarState();
    g_barWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"ZpixSelectBar", L"", WS_POPUP, 0, 0, 88, 40, NULL, NULL,
                               GetModuleHandleW(NULL), NULL);
    if (!g_barWnd) {
        delete g_bar;
        g_bar = nullptr;
        return;
    }
    g_bar->hwnd = g_barWnd;
    EnableRoundedBar(g_barWnd);
}

static void ShowSelectBar(const std::wstring& text, POINT anchor) {
    if (!g_cfg.enableSelectTranslate || IsOverlayActive()) return;
    EnsureSelectBarWindow();
    if (!g_bar || !g_barWnd) return;

    g_bar->scale = MonitorScaleOf(anchor);
    g_bar->width = BS(88);
    g_bar->height = BS(38);
    g_bar->text = text;
    g_bar->anchor = anchor;
    g_bar->hover = 0;
    g_bar->pressed = 0;

    int pad = BS(4);
    int half = g_bar->width / 2;
    g_bar->rcCopy = RECT{pad, pad, half - BS(2), g_bar->height - pad};
    g_bar->rcTrans = RECT{half + BS(2), pad, g_bar->width - pad, g_bar->height - pad};

    RECT work = MonitorWorkRect(anchor);
    int x = anchor.x - g_bar->width / 2;
    int y = anchor.y + BS(14);
    if (y + g_bar->height > work.bottom - BS(8)) y = anchor.y - g_bar->height - BS(14);
    if (x < work.left + BS(6)) x = work.left + BS(6);
    if (x + g_bar->width > work.right - BS(6)) x = work.right - g_bar->width - BS(6);
    if (y < work.top + BS(6)) y = work.top + BS(6);

    SetWindowPos(g_barWnd, HWND_TOPMOST, x, y, g_bar->width, g_bar->height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_barWnd, NULL, FALSE);
    UpdateWindow(g_barWnd);
    SetTimer(g_barWnd, kTimerAutoHide, 7500, NULL);
}

// ---------------------------------------------------------------- 无损剪贴板备份 / 恢复 / 模拟 Ctrl+C

struct SavedClipFmt {
    UINT fmt = 0;
    std::vector<BYTE> bytes;
};

static bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 6; ++i) {
        if (OpenClipboard(owner)) return true;
        Sleep(10);
    }
    return false;
}

static bool BackupClipboard(std::vector<SavedClipFmt>& saved) {
    saved.clear();
    if (!OpenClipboardRetry(NULL)) return false;

    UINT pngFmt = RegisterClipboardFormatW(L"PNG");
    UINT htmlFmt = RegisterClipboardFormatW(L"HTML Format");
    UINT rtfFmt = RegisterClipboardFormatW(L"Rich Text Format");
    const UINT fmts[] = {CF_UNICODETEXT, CF_HDROP, pngFmt, CF_DIBV5, CF_DIB, htmlFmt, rtfFmt};
    for (UINT f : fmts) {
        if (!f || !IsClipboardFormatAvailable(f)) continue;
        HANDLE h = GetClipboardData(f);
        if (!h) continue;
        SIZE_T sz = GlobalSize(h);
        if (sz == 0 || sz > 32u * 1024u * 1024u) continue;
        void* p = GlobalLock(h);
        if (p) {
            SavedClipFmt item;
            item.fmt = f;
            item.bytes.assign((const BYTE*)p, (const BYTE*)p + sz);
            saved.push_back(std::move(item));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return true;
}

static void RestoreClipboard(const std::vector<SavedClipFmt>& saved) {
    if (!OpenClipboardRetry(NULL)) return;
    EmptyClipboard();
    for (const auto& item : saved) {
        if (item.bytes.empty()) continue;
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, item.bytes.size());
        if (!h) continue;
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, item.bytes.data(), item.bytes.size());
            GlobalUnlock(h);
            if (!SetClipboardData(item.fmt, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

static void SendCtrlC() {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[0].ki.wScan = (WORD)MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC);

    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = 'C';
    in[1].ki.wScan = (WORD)MapVirtualKeyW('C', MAPVK_VK_TO_VSC);

    in[2] = in[1];
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;

    in[3] = in[0];
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(4, in, sizeof(INPUT));
}

static std::wstring TrimSelectedText(std::wstring s) {
    while (!s.empty() &&
           (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n'))
        s.erase(s.begin());
    while (!s.empty() &&
           (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n'))
        s.pop_back();
    return s;
}

static bool ShouldIgnoreWindow(POINT downPt, POINT upPt) {
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId()) return true;
    }

    HWND wDown = WindowFromPoint(downPt);
    HWND wUp = WindowFromPoint(upPt);
    if (wUp) {
        DWORD pid = 0;
        GetWindowThreadProcessId(wUp, &pid);
        if (pid == GetCurrentProcessId()) return true;
    }

    HWND root = wDown ? GetAncestor(wDown, GA_ROOT) : fg;
    wchar_t rootCls[128] = {}, childCls[128] = {};
    if (root) GetClassNameW(root, rootCls, 128);
    if (wDown) GetClassNameW(wDown, childCls, 128);

    // 跳过终端/命令行窗口（防止模拟 Ctrl+C 发送 SIGINT 中断正在运行的命令）
    static const wchar_t* kSkipConsole[] = {
        L"ConsoleWindowClass",
        L"CASCADIA_HOSTING_WINDOW_CLASS",
        L"VirtualConsoleClass",
        L"mintty",
        L"PuTTY",
    };
    for (const wchar_t* sk : kSkipConsole) {
        if (_wcsicmp(rootCls, sk) == 0 || _wcsicmp(childCls, sk) == 0) return true;
    }

    // 跳过桌面、任务栏、菜单和资源管理器文件列表（拖选文件不弹翻译）
    static const wchar_t* kSkipRoot[] = {
        L"Progman",          L"WorkerW",
        L"Shell_TrayWnd",    L"Shell_SecondaryTrayWnd",
        L"CabinetWClass",    L"ExploreWClass",
        L"#32768",           L"MultitaskingViewFrame",
        L"NotifyIconOverflowWindow",
    };
    for (const wchar_t* sk : kSkipRoot) {
        if (_wcsicmp(rootCls, sk) == 0) {
            // 如果是在资源管理器地址栏/搜索框里编辑文字，仍放行
            if (_wcsicmp(childCls, L"Edit") != 0 && _wcsicmp(childCls, L"RichEditD2DPT") != 0)
                return true;
        }
    }

    // 跳过拖拽标题栏、窗口边框、滚动条、系统按钮
    if (wDown) {
        DWORD_PTR ht = 0;
        if (SendMessageTimeoutW(wDown, WM_NCHITTEST, 0, MAKELPARAM(downPt.x, downPt.y),
                                SMTO_ABORTIFHUNG, 30, &ht)) {
            if (ht == HTCAPTION || ht == HTSYSMENU || ht == HTMINBUTTON || ht == HTMAXBUTTON ||
                ht == HTCLOSE || ht == HTHSCROLL || ht == HTVSCROLL ||
                (ht >= HTLEFT && ht <= HTBOTTOMRIGHT)) {
                return true;
            }
        }
    }
    return false;
}

static void LaunchSelectionCapture(POINT downPt, POINT upPt) {
    if (!g_cfg.enableSelectTranslate || IsOverlayActive()) return;
    if (ShouldIgnoreWindow(downPt, upPt)) return;
    if (g_capturing.exchange(true)) return;

    std::thread([upPt]() {
        // 等目标应用（浏览器 / 微信 / Electron）完成选区提交
        Sleep(45);

        // 若用户此时又按下了鼠标或修饰键，放弃本次捕获（防止 Shift+Ctrl+C 触发 DevTools 等）
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
            (GetAsyncKeyState(VK_MENU) & 0x8000) || (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
            (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
            g_capturing = false;
            return;
        }

        std::vector<SavedClipFmt> backup;
        bool backupOk = BackupClipboard(backup);
        DWORD seqBefore = GetClipboardSequenceNumber();
        SendCtrlC();

        bool changed = false;
        for (int i = 0; i < 14; ++i) {
            Sleep(12);
            if (GetClipboardSequenceNumber() != seqBefore) {
                changed = true;
                break;
            }
        }

        std::wstring captured;
        if (changed) {
            bool isFile = false;
            if (OpenClipboardRetry(NULL)) {
                if (IsClipboardFormatAvailable(CF_HDROP)) {
                    isFile = true;
                } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    if (h) {
                        const wchar_t* p = (const wchar_t*)GlobalLock(h);
                        if (p) {
                            captured = p;
                            GlobalUnlock(h);
                        }
                    }
                }
                CloseClipboard();
            }
            // 立刻无损恢复用户原有的剪贴板内容
            if (backupOk) RestoreClipboard(backup);
            if (isFile) captured.clear();
        }

        captured = TrimSelectedText(std::move(captured));
        if (!captured.empty() && captured.size() <= 8000 && g_hostWnd && IsWindow(g_hostWnd)) {
            SelectReadyPayload* pl = new SelectReadyPayload();
            pl->text = std::move(captured);
            pl->anchor = upPt;
            PostMessageW(g_hostWnd, WM_ZPIX_SELECT_READY, 0, (LPARAM)pl);
        }
        g_capturing = false;
    }).detach();
}

// ---------------------------------------------------------------- 低级鼠标钩子（<0.1ms 返回）

static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && lp) {
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lp;
        POINT pt = ms->pt;

        if (wp == WM_LBUTTONDOWN) {
            bool onBar = false;
            if (g_barWnd && IsWindowVisible(g_barWnd)) {
                RECT br{};
                if (GetWindowRect(g_barWnd, &br) && PtInRect(&br, pt)) {
                    onBar = true;
                } else if (g_hostWnd) {
                    PostMessageW(g_hostWnd, WM_ZPIX_SELECT_HIDE, 0, 0);
                }
            }
            if (!onBar && g_cfg.enableSelectTranslate && !IsOverlayActive()) {
                g_downValid = true;
                g_downPt = pt;
                g_downTick = GetTickCount();
            } else {
                g_downValid = false;
            }
        } else if (wp == WM_RBUTTONDOWN || wp == WM_MOUSEWHEEL) {
            if (g_barWnd && IsWindowVisible(g_barWnd) && g_hostWnd) {
                PostMessageW(g_hostWnd, WM_ZPIX_SELECT_HIDE, 0, 0);
            }
            g_downValid = false;
        } else if (wp == WM_LBUTTONUP && g_downValid) {
            g_downValid = false;
            if (g_cfg.enableSelectTranslate && !IsOverlayActive()) {
                bool modHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                               (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                               (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                               (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                               (GetAsyncKeyState(VK_RWIN) & 0x8000);
                if (!modHeld) {
                    DWORD now = GetTickCount();
                    int dx = std::abs(pt.x - g_downPt.x);
                    int dy = std::abs(pt.y - g_downPt.y);
                    DWORD dur = now - g_downTick;

                    bool isDrag = (dx >= 8 || dy >= 8) && dur >= 50 && dur <= 15000;
                    bool isDblClick =
                        !isDrag && !g_lastWasDrag && dx <= 5 && dy <= 5 &&
                        (now - g_lastUpTick) <= GetDoubleClickTime() &&
                        std::abs(pt.x - g_lastUpPt.x) <= 6 && std::abs(pt.y - g_lastUpPt.y) <= 6;

                    g_lastUpPt = pt;
                    g_lastUpTick = now;
                    g_lastWasDrag = isDrag;

                    if ((isDrag || isDblClick) && g_hostWnd) {
                        {
                            std::lock_guard<std::mutex> lk(g_gestureMu);
                            g_pendingDownPt = g_downPt;
                            g_pendingUpPt = pt;
                        }
                        PostMessageW(g_hostWnd, WM_ZPIX_SELECT_TRIGGER, 0, 0);
                    }
                }
            }
        }
    }
    return CallNextHookEx(g_mouseHook, code, wp, lp);
}

}  // namespace

void StartSelectMonitor(HWND msgWnd) {
    g_hostWnd = msgWnd;
    if (!g_mouseHook) {
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, GetModuleHandleW(NULL), 0);
        ZLog(L"[sel] mouse hook installed=%d", g_mouseHook ? 1 : 0);
    }
}

void StopSelectMonitor() {
    HideSelectBar();
    if (g_barWnd && IsWindow(g_barWnd)) {
        DestroyWindow(g_barWnd);
        g_barWnd = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

void HideSelectBar() {
    if (g_barWnd && IsWindow(g_barWnd)) {
        KillTimer(g_barWnd, kTimerAutoHide);
        ShowWindow(g_barWnd, SW_HIDE);
    }
}

void HandleSelectMessage(UINT msg, WPARAM, LPARAM lp) {
    switch (msg) {
    case WM_ZPIX_SELECT_HIDE:
        HideSelectBar();
        break;
    case WM_ZPIX_SELECT_TRIGGER: {
        POINT d{}, u{};
        {
            std::lock_guard<std::mutex> lk(g_gestureMu);
            d = g_pendingDownPt;
            u = g_pendingUpPt;
        }
        LaunchSelectionCapture(d, u);
        break;
    }
    case WM_ZPIX_SELECT_READY: {
        SelectReadyPayload* pl = (SelectReadyPayload*)lp;
        if (pl) {
            if (!pl->text.empty()) ShowSelectBar(pl->text, pl->anchor);
            delete pl;
        }
        break;
    }
    }
}
