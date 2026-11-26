#include "overlay.h"
#include "actions.h"
#include "annotate.h"
#include "config.h"
#include "img_search.h"
#include "img_translate.h"
#include "pin.h"
#include "select_translate.h"
#include "toolbar_icons.h"
#include "ui.h"
#include "util.h"

#include <uiautomation.h>
#include <oleacc.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

const COLORREF kAccent = RGB(58, 130, 246);
const COLORREF kToolbarBorder = RGB(60, 64, 67);

// 截图操作栏白底配色（叠在截图上不抢画面，热键做上标）
const COLORREF kBarBg = RGB(255, 255, 255);
const COLORREF kBarBorder = RGB(214, 217, 222);
const COLORREF kBarSep = RGB(230, 232, 237);
const COLORREF kBarBtnBg = RGB(255, 255, 255);
const COLORREF kBarHover = RGB(242, 243, 246);
const COLORREF kBarActiveBg = RGB(255, 244, 230);
const COLORREF kBarAccent = RGB(240, 144, 21);
const COLORREF kBarAccentDark = RGB(214, 120, 8);
const COLORREF kBarText = RGB(38, 40, 44);
const COLORREF kBarDim = RGB(158, 162, 168);

// 6 种常用标注颜色：红、蓝、绿、黄、白、黑
const COLORREF kPalette[6] = {
    RGB(239, 68, 68),   // 红
    RGB(59, 130, 246),  // 蓝
    RGB(34, 197, 94),   // 绿
    RGB(250, 204, 21),  // 黄
    RGB(255, 255, 255), // 白
    RGB(24, 24, 27),    // 黑
};
const int kStrokeWidths[3] = {2, 4, 6};
const int kTextSizes[3] = {16, 22, 32};
const int kMosaicBlocks[3] = {6, 10, 16};
const wchar_t* kPickerFmtLabels[6] = {L"HEX", L"HEXA", L"RGB", L"RGBA", L"HSL", L"HSLA"};

enum BtnId {
    BTN_COPY = 1,
    BTN_PIN,
    BTN_SAVE,
    BTN_OCR,
    BTN_TRANSLATE,
    BTN_IMG_TRANSLATE,
    BTN_IMG_SEARCH,
    BTN_ARROW,
    BTN_RECT,
    BTN_TEXT,
    BTN_MOSAIC,
    BTN_PICKER,
    BTN_CLOSE
};

enum Tool { TOOL_NONE = 0, TOOL_ARROW, TOOL_RECT, TOOL_TEXT, TOOL_MOSAIC, TOOL_PICKER };

enum SelHit {
    HIT_NONE = 0,
    HIT_MOVE,
    HIT_TL,
    HIT_T,
    HIT_TR,
    HIT_R,
    HIT_BR,
    HIT_B,
    HIT_BL,
    HIT_L
};

enum SubKind { SUB_NONE = 0, SUB_SIZE, SUB_COLOR, SUB_PICKFMT, SUB_IMGENGINE };

struct Btn {
    int id = 0;
    int iconIdx = -1;
    std::wstring label;
    std::wstring key;
    RECT rc{};
};

struct SubBtn {
    int id = 0;
    SubKind kind = SUB_NONE;
    int value = 0;
    COLORREF color = 0;
    std::wstring label;
    RECT rc{};
};

const UINT WM_ZPIX_UIA_READY = WM_APP + 31;
const UINT WM_ZPIX_IMGTR_DONE = WM_APP + 32;

struct ImgTrShared {
    std::atomic<bool> alive{true};
    HWND notifyHwnd = nullptr;
};

struct ImgTrAsyncMsg {
    uint64_t seq = 0;
    bool ok = false;
    img_translate::Result res;
};

struct TopWinEntry {
    HWND hwnd = nullptr;
    RECT rcClient{};              // 覆盖层客户区坐标
    std::vector<RECT> subRects;   // 子 HWND + UIA DOM/控件矩形（客户区坐标）
    bool uiaQueued = false;
};

struct UiaBatchResult {
    HWND topHwnd = nullptr;
    std::vector<RECT> rects;
};

struct UiaSharedState {
    std::atomic<bool> alive{true};
    HWND notifyHwnd = nullptr;
    RECT vs{};
};

struct OverlayState {
    HWND hwnd = nullptr;
    Bitmap32 shot;
    Bitmap32 dim;
    HDC shotDC = nullptr;
    HDC dimDC = nullptr;
    HDC backDC = nullptr;
    HBITMAP shotBmp = nullptr;
    HBITMAP dimBmp = nullptr;
    HBITMAP backBmp = nullptr;
    HGDIOBJ shotOld = nullptr;
    HGDIOBJ dimOld = nullptr;
    HGDIOBJ backOld = nullptr;
    RECT vs{};

    int phase = 0;  // 0=待选择(悬停识窗) 1=拖拽新建选区 2=已选中
    POINT p0{}, p1{};
    RECT sel{};     // 客户区坐标
    bool dragMoved = false;

    // 选区平移 / 8 方向缩放
    int selDragMode = HIT_NONE;
    bool selDragMoved = false;
    POINT selDragStart{};
    RECT selOrig{};
    bool reselectPending = false;

    // 智能窗口 / 子元素 / 视觉卡片多级识别
    std::vector<TopWinEntry> topWins;
    std::vector<RECT> monRects;
    std::vector<RECT> hoverStack;  // 当前光标下由小到大的候选矩形栈
    int hoverLevel = 0;            // 滚轮切换层级：0=最小子元素，向上滚放大
    RECT lastSmallestElem{};
    std::shared_ptr<UiaSharedState> uiaState;
    RECT hoverWin{};
    bool hasHoverWin = false;
    RECT hintMonRect{};

    // 标注与属性
    std::vector<Ann> items;
    int tool = TOOL_NONE;
    bool drawing = false;
    Ann draft;
    int colorIdx = 0;       // 0..5 -> kPalette
    int strokeIdx = 1;      // 0..2 -> 2 / 4 / 6
    int textSizeIdx = 1;    // 0..2 -> 16 / 22 / 32
    int mosaicIdx = 1;      // 0..2 -> 6 / 10 / 16
    int pickerFmt = 0;      // 0..5 -> HEX/HEXA/RGB/RGBA/HSL/HSLA

    // 主工具栏 & 二级属性条
    std::vector<Btn> btns;
    std::vector<int> barSepXs;
    int hover = 0;
    int pressed = 0;

    std::vector<SubBtn> subBtns;
    RECT subBarRect{};
    int subSepX = 0;
    int hoverSub = 0;
    int pressedSub = 0;
    bool imgSearchSubOpen = false;
    RECT tipRect{};

    // 取色放大镜
    POINT mousePt{};
    bool hasMousePt = false;
    RECT loupeRect{};

    // 透明背景实时文字编辑
    HWND hEdit = nullptr;
    POINT editPosScreen{};
    std::wstring editText;
    int editCaret = 0;
    bool editCaretOn = true;
    bool keepEditOpen = false;
    RECT editVisualRect{};

    // 图内原位翻译状态与位图缓存
    std::shared_ptr<ImgTrShared> imgTrShared;
    uint64_t imgTrSeq = 0;
    bool imgTrBusy = false;
    bool imgTrHasCache = false;
    bool imgTrActive = false;
    RECT imgTrSel{};
    Bitmap32 imgTrPatch;
    HDC imgTrDC = nullptr;
    HBITMAP imgTrBmp = nullptr;
    HGDIOBJ imgTrOld = nullptr;
    std::wstring imgTrNote;

    HFONT fontBtn = nullptr;
    HFONT fontKey = nullptr;
    HFONT fontDim = nullptr;
    HFONT fontHint = nullptr;
    HFONT fontSmall = nullptr;
};

OverlayState* g_ov = nullptr;

const UINT_PTR kTimerCaret = 2;

RECT ToolbarRect();
void LayoutToolbar();
void LayoutSubToolbar();
void CommitEdit(bool keep);

RECT SelScreenRect() {
    RECT r = g_ov->sel;
    OffsetRect(&r, g_ov->vs.left, g_ov->vs.top);
    return r;
}

void DrawRoundRect(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border) {
    ui::RoundRect(dc, r, radius / 2 > 0 ? radius / 2 : radius, fill, border);
}

// ---------------------------------------------------------------- 智能窗口识别

static bool QueryDwmWindowAttr(HWND hwnd, DWORD attr, void* out, DWORD size) {
    typedef HRESULT(WINAPI * PFN)(HWND, DWORD, PVOID, DWORD);
    static PFN pfn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        if (m) pfn = (PFN)GetProcAddress(m, "DwmGetWindowAttribute");
    }
    return pfn && SUCCEEDED(pfn(hwnd, attr, out, size));
}

struct EnumWinCtx {
    RECT vs;
    std::vector<TopWinEntry>* wins;
    std::vector<RECT>* mons;
};

static bool NearEqualRect(const RECT& a, const RECT& b, int tol = 4) {
    return std::abs(a.left - b.left) <= tol && std::abs(a.top - b.top) <= tol &&
           std::abs(a.right - b.right) <= tol && std::abs(a.bottom - b.bottom) <= tol;
}

static void AppendUniqueRect(std::vector<RECT>& vec, const RECT& r, int tol = 3) {
    if (r.right - r.left < 18 || r.bottom - r.top < 14) return;
    for (const RECT& ex : vec) {
        if (NearEqualRect(ex, r, tol)) return;
    }
    vec.push_back(r);
}

struct ChildEnumCtx {
    RECT vs;
    RECT topClip;
    std::vector<RECT>* out;
};

static BOOL CALLBACK CollectChildProc(HWND ch, LPARAM lp) {
    ChildEnumCtx* c = (ChildEnumCtx*)lp;
    if (!IsWindowVisible(ch)) return TRUE;
    RECT r{};
    if (!GetWindowRect(ch, &r)) return TRUE;
    OffsetRect(&r, -c->vs.left, -c->vs.top);
    RECT clipped{};
    if (!IntersectRect(&clipped, &r, &c->topClip)) return TRUE;
    if (clipped.right - clipped.left < 18 || clipped.bottom - clipped.top < 14) return TRUE;
    if (!NearEqualRect(clipped, c->topClip, 4) && c->out->size() < 400) {
        AppendUniqueRect(*c->out, clipped, 3);
    }
    return TRUE;
}

static BOOL CALLBACK CollectWindowProc(HWND hwnd, LPARAM lp) {
    EnumWinCtx* ctx = (EnumWinCtx*)lp;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

    DWORD cloaked = 0;
    if (QueryDwmWindowAttr(hwnd, 14, &cloaked, sizeof(cloaked)) && cloaked != 0) return TRUE;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, NULL, &alpha, &flags) && (flags & LWA_ALPHA) &&
            alpha == 0) {
            return TRUE;
        }
    }
    if ((ex & WS_EX_TOOLWINDOW) && (ex & WS_EX_TRANSPARENT)) return TRUE;

    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    static const wchar_t* kSkipClasses[] = {
        L"ZpixOverlay",
        L"ZpixToast",
        L"ZpixSelectBar",
        L"Progman",
        L"WorkerW",
        L"ForegroundStaging",
        L"ApplicationManager_DesktopShellWindow",
        L"Windows.UI.Core.CoreWindow",
        L"EdgeUiInputTopWndClass",
        L"EdgeUiInputWndClass",
        L"MultitaskingViewFrame",
        L"Shell_Dim",
    };
    for (const wchar_t* skip : kSkipClasses) {
        if (_wcsicmp(cls, skip) == 0) return TRUE;
    }

    RECT r{};
    if (!QueryDwmWindowAttr(hwnd, 9, &r, sizeof(r))) {
        if (!GetWindowRect(hwnd, &r)) return TRUE;
    }
    if (r.right - r.left < 16 || r.bottom - r.top < 16) return TRUE;

    if (IsZoomed(hwnd)) {
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (mon) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) IntersectRect(&r, &r, &mi.rcMonitor);
        }
    }

    RECT clipped{};
    if (!IntersectRect(&clipped, &r, &ctx->vs)) return TRUE;
    if (clipped.right - clipped.left < 16 || clipped.bottom - clipped.top < 16) return TRUE;

    OffsetRect(&clipped, -ctx->vs.left, -ctx->vs.top);

    TopWinEntry entry;
    entry.hwnd = hwnd;
    entry.rcClient = clipped;
    ChildEnumCtx cctx{ctx->vs, clipped, &entry.subRects};
    EnumChildWindows(hwnd, CollectChildProc, (LPARAM)&cctx);
    ctx->wins->push_back(std::move(entry));
    return TRUE;
}

static BOOL CALLBACK CollectMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    EnumWinCtx* ctx = (EnumWinCtx*)lp;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    RECT clipped{};
    if (!IntersectRect(&clipped, &mi.rcMonitor, &ctx->vs)) return TRUE;
    OffsetRect(&clipped, -ctx->vs.left, -ctx->vs.top);
    ctx->mons->push_back(clipped);
    return TRUE;
}

// 后台异步提取窗口的 UI Automation 树（网页 div / 按钮 / 微信 4.0 Qt 气泡 / Electron 面板）
static void WalkCachedUiaTree(IUIAutomationElement* el, int depth, const RECT& vs,
                              const RECT& topClip, std::vector<RECT>& out) {
    if (!el || depth > 32 || out.size() >= 2800) return;
    BOOL off = FALSE;
    el->get_CachedIsOffscreen(&off);
    RECT r{};
    if (!off && SUCCEEDED(el->get_CachedBoundingRectangle(&r))) {
        if (r.right - r.left >= 18 && r.bottom - r.top >= 14) {
            OffsetRect(&r, -vs.left, -vs.top);
            RECT cl{};
            if (IntersectRect(&cl, &r, &topClip) && (cl.right - cl.left >= 18) &&
                (cl.bottom - cl.top >= 14) && !NearEqualRect(cl, topClip, 4)) {
                AppendUniqueRect(out, cl, 3);
            }
        }
    }
    IUIAutomationElementArray* kids = nullptr;
    if (SUCCEEDED(el->GetCachedChildren(&kids)) && kids) {
        int n = 0;
        kids->get_Length(&n);
        for (int i = 0; i < n && out.size() < 2800; ++i) {
            IUIAutomationElement* ch = nullptr;
            if (SUCCEEDED(kids->GetElement(i, &ch)) && ch) {
                WalkCachedUiaTree(ch, depth + 1, vs, topClip, out);
                ch->Release();
            }
        }
        kids->Release();
    }
}

static void QueueUiaForWindow(OverlayState* st, TopWinEntry& entry) {
    if (!st || !st->uiaState || entry.uiaQueued || !entry.hwnd) return;
    entry.uiaQueued = true;
    std::shared_ptr<UiaSharedState> shared = st->uiaState;
    HWND topHwnd = entry.hwnd;
    RECT topClip = entry.rcClient;

    std::thread([shared, topHwnd, topClip]() {
        if (!shared->alive.load()) return;
        DWORD_PTR ping = 0;
        if (!SendMessageTimeoutW(topHwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 45, &ping)) return;

        CoInitializeEx(NULL, COINIT_MULTITHREADED);

        // 唤醒 Chromium / Edge / Electron / CEF 网页 DOM 的惰性 Accessibility Tree
        bool wokeChromium = false;
        auto wakeOne = [&](HWND target) {
            DWORD_PTR ok = 0;
            if (!SendMessageTimeoutW(target, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 35, &ok)) return;
            IAccessible* acc = nullptr;
            if (SUCCEEDED(AccessibleObjectFromWindow(target, OBJID_CLIENT, IID_IAccessible,
                                                     (void**)&acc)) &&
                acc) {
                long childCnt = 0;
                acc->get_accChildCount(&childCnt);
                VARIANT vSelf{};
                vSelf.vt = VT_I4;
                vSelf.lVal = CHILDID_SELF;
                BSTR bName = nullptr;
                acc->get_accName(vSelf, &bName);
                if (bName) SysFreeString(bName);
                if (childCnt <= 1) wokeChromium = true;
                acc->Release();
            }
        };

        wchar_t rcls[128] = {};
        GetClassNameW(topHwnd, rcls, 128);
        if (wcsstr(rcls, L"Chrome_WidgetWin") || wcsstr(rcls, L"WeChat") || wcsstr(rcls, L"Qt")) {
            wakeOne(topHwnd);
        }
        EnumChildWindows(
            topHwnd,
            [](HWND ch, LPARAM lp) -> BOOL {
                bool* woke = (bool*)lp;
                if (!IsWindowVisible(ch)) return TRUE;
                wchar_t ccls[128] = {};
                GetClassNameW(ch, ccls, 128);
                if (wcsstr(ccls, L"Chrome_RenderWidgetHostHWND") ||
                    wcsstr(ccls, L"CefBrowserWindow") || wcsstr(ccls, L"WebView")) {
                    DWORD_PTR ok = 0;
                    if (SendMessageTimeoutW(ch, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 35, &ok)) {
                        IAccessible* acc = nullptr;
                        if (SUCCEEDED(AccessibleObjectFromWindow(ch, OBJID_CLIENT, IID_IAccessible,
                                                                 (void**)&acc)) &&
                            acc) {
                            long cnt = 0;
                            acc->get_accChildCount(&cnt);
                            if (cnt <= 1) *woke = true;
                            acc->Release();
                        }
                    }
                }
                return TRUE;
            },
            (LPARAM)&wokeChromium);

        if (wokeChromium && shared->alive.load()) {
            // 给浏览器渲染进程一帧时间生成完整 DOM AXTree（如 div / pre / img / button）
            Sleep(45);
        }
        if (!shared->alive.load()) {
            CoUninitialize();
            return;
        }

        std::vector<RECT> found;
        IUIAutomation* uia = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                       IID_IUIAutomation, (void**)&uia)) &&
            uia) {
            IUIAutomationCacheRequest* req = nullptr;
            if (SUCCEEDED(uia->CreateCacheRequest(&req)) && req) {
                req->AddProperty(UIA_BoundingRectanglePropertyId);
                req->AddProperty(UIA_IsOffscreenPropertyId);
                req->put_TreeScope((TreeScope)(TreeScope_Element | TreeScope_Descendants));
                req->put_AutomationElementMode(AutomationElementMode_None);

                IUIAutomationElement* root = nullptr;
                if (SUCCEEDED(uia->ElementFromHandleBuildCache((UIA_HWND)topHwnd, req, &root)) &&
                    root) {
                    WalkCachedUiaTree(root, 0, shared->vs, topClip, found);
                    root->Release();
                }
                req->Release();
            }
            uia->Release();
        }
        CoUninitialize();

        if (!found.empty() && shared->alive.load() && shared->notifyHwnd &&
            IsWindow(shared->notifyHwnd)) {
            UiaBatchResult* batch = new UiaBatchResult();
            batch->topHwnd = topHwnd;
            batch->rects = std::move(found);
            if (!PostMessageW(shared->notifyHwnd, WM_ZPIX_UIA_READY, 0, (LPARAM)batch)) {
                delete batch;
            }
        }
    }).detach();
}

// ---------------------------------------------------------------- 像素级视觉分栏 & 气泡/卡片检测（0.05ms 级）
// 兼容自绘界面（如微信聊天列表/聊天窗/输入框/单条消息气泡、网页无障碍未标注的代码块/卡片/图片）

static inline int PixDiff(uint32_t a, uint32_t b) {
    int dr = std::abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF));
    int dg = std::abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF));
    int db = std::abs((int)(a & 0xFF) - (int)(b & 0xFF));
    return std::max(dr, std::max(dg, db));
}

static inline uint32_t FastPx(const Bitmap32& bm, int x, int y) {
    if (x < 0) x = 0;
    else if (x >= bm.w) x = bm.w - 1;
    if (y < 0) y = 0;
    else if (y >= bm.h) y = bm.h - 1;
    return bm.px[(size_t)y * bm.w + x];
}

// 检测贯穿局部窗口的水平/垂直分割线或双色分栏边界（如微信左栏|会话列表|聊天头|聊天内容|输入框）
static bool IsVerticalSeam(const Bitmap32& bm, int x, int y0, int y1) {
    int span = y1 - y0;
    if (span < 64 || x <= 1 || x >= bm.w - 2) return false;
    const int N = 20;
    uint32_t cMid0 = FastPx(bm, x, y0 + span / 2);
    uint32_t cL0 = FastPx(bm, x - 2, y0 + span / 2);
    uint32_t cR0 = FastPx(bm, x + 2, y0 + span / 2);
    // 快速短路：中点既不是分割线也不是双色跳变则立即跳过（仅 3 次内存读）
    if (PixDiff(cMid0, cL0) < 6 && PixDiff(cMid0, cR0) < 6 && PixDiff(cL0, cR0) < 10) return false;

    int lineHits = 0, stepHits = 0;
    for (int i = 0; i < N; ++i) {
        int y = y0 + (span * (i * 2 + 1)) / (N * 2);
        uint32_t cm = FastPx(bm, x, y);
        uint32_t cl = FastPx(bm, x - 2, y);
        uint32_t cr = FastPx(bm, x + 2, y);
        if (PixDiff(cm, cMid0) <= 5 && (PixDiff(cm, cl) >= 7 || PixDiff(cm, cr) >= 7)) ++lineHits;
        if (PixDiff(cl, cL0) <= 5 && PixDiff(cr, cR0) <= 5 && PixDiff(cl, cr) >= 10) ++stepHits;
    }
    return lineHits >= 16 || stepHits >= 16;
}

static bool IsHorizontalSeam(const Bitmap32& bm, int y, int x0, int x1) {
    int span = x1 - x0;
    if (span < 64 || y <= 1 || y >= bm.h - 2) return false;
    const int N = 20;
    uint32_t cMid0 = FastPx(bm, x0 + span / 2, y);
    uint32_t cT0 = FastPx(bm, x0 + span / 2, y - 2);
    uint32_t cB0 = FastPx(bm, x0 + span / 2, y + 2);
    if (PixDiff(cMid0, cT0) < 6 && PixDiff(cMid0, cB0) < 6 && PixDiff(cT0, cB0) < 10) return false;

    int lineHits = 0, stepHits = 0;
    for (int i = 0; i < N; ++i) {
        int x = x0 + (span * (i * 2 + 1)) / (N * 2);
        uint32_t cm = FastPx(bm, x, y);
        uint32_t ct = FastPx(bm, x, y - 2);
        uint32_t cb = FastPx(bm, x, y + 2);
        if (PixDiff(cm, cMid0) <= 5 && (PixDiff(cm, ct) >= 7 || PixDiff(cm, cb) >= 7)) ++lineHits;
        if (PixDiff(ct, cT0) <= 5 && PixDiff(cb, cB0) <= 5 && PixDiff(ct, cb) >= 10) ++stepHits;
    }
    return lineHits >= 16 || stepHits >= 16;
}

// 检测浮在纯色底上的独立视觉卡片 / 微信消息气泡 / 网页代码块 / 按钮 / 图片框
static void DetectVisualRegionsAt(const Bitmap32& bm, const RECT& winRc, POINT pt,
                                  std::vector<RECT>& out) {
    if (bm.empty() || !PtInRect(&winRc, pt)) return;

    // 1) 寻找光标所在的分栏面板（先找左右竖分栏，再在分栏内找上下横分栏）
    int ySample0 = std::max<int>(winRc.top + 8, pt.y - 160);
    int ySample1 = std::min<int>(winRc.bottom - 8, pt.y + 160);
    int paneL = winRc.left, paneR = winRc.right;
    for (int x = pt.x - 4; x > winRc.left + 24; --x) {
        if (IsVerticalSeam(bm, x, ySample0, ySample1)) {
            paneL = x;
            break;
        }
    }
    for (int x = pt.x + 4; x < winRc.right - 24; ++x) {
        if (IsVerticalSeam(bm, x, ySample0, ySample1)) {
            paneR = x;
            break;
        }
    }
    if (paneR - paneL >= 48 && (paneL > winRc.left + 16 || paneR < winRc.right - 16)) {
        RECT colPane{paneL, winRc.top, paneR, winRc.bottom};
        AppendUniqueRect(out, colPane, 4);
    }

    int xSample0 = std::max<int>(paneL + 8, pt.x - 220);
    int xSample1 = std::min<int>(paneR - 8, pt.x + 220);
    if (paneR - paneL <= 440) {
        xSample0 = paneL + 8;
        xSample1 = paneR - 8;
    }
    int paneT = winRc.top, paneB = winRc.bottom;
    for (int y = pt.y - 4; y > winRc.top + 20; --y) {
        if (IsHorizontalSeam(bm, y, xSample0, xSample1)) {
            paneT = y;
            break;
        }
    }
    for (int y = pt.y + 4; y < winRc.bottom - 20; ++y) {
        if (IsHorizontalSeam(bm, y, xSample0, xSample1)) {
            paneB = y;
            break;
        }
    }
    RECT subPane{paneL, paneT, paneR, paneB};
    if (subPane.right - subPane.left >= 48 && subPane.bottom - subPane.top >= 28 &&
        !NearEqualRect(subPane, winRc, 8)) {
        AppendUniqueRect(out, subPane, 4);
    }

    // 2) 在当前面板内探测被纯色背景包围的卡片 / 聊天气泡 / 代码块 / 图片
    auto findMoatEdge = [&](int startX, int startY, int dx, int dy, int limit,
                            uint32_t* moatColor) -> int {
        const int runNeed = 7;
        int run = 0;
        uint32_t ref = FastPx(bm, startX, startY);
        int edgePos = -1;
        for (int s = 1; s <= limit; ++s) {
            int cx = startX + dx * s;
            int cy = startY + dy * s;
            if (cx <= subPane.left + 1 || cx >= subPane.right - 2 || cy <= subPane.top + 1 ||
                cy >= subPane.bottom - 2)
                break;
            uint32_t c = FastPx(bm, cx, cy);
            if (run == 0) {
                uint32_t prev = FastPx(bm, cx - dx, cy - dy);
                if (PixDiff(c, prev) >= 5) {
                    ref = c;
                    run = 1;
                    edgePos = (dx != 0) ? cx : cy;
                }
            } else {
                if (PixDiff(c, ref) <= 4) {
                    ++run;
                    if (run >= runNeed) {
                        *moatColor = ref;
                        return edgePos;
                    }
                } else {
                    if (PixDiff(c, FastPx(bm, cx - dx, cy - dy)) >= 5) {
                        ref = c;
                        run = 1;
                        edgePos = (dx != 0) ? cx : cy;
                    } else {
                        run = 0;
                    }
                }
            }
        }
        return -1;
    };

    uint32_t cL = 0, cR = 0, cT = 0, cB = 0;
    int eL = findMoatEdge(pt.x, pt.y, -1, 0, 560, &cL);
    int eR = findMoatEdge(pt.x, pt.y, +1, 0, 560, &cR);
    int eT = findMoatEdge(pt.x, pt.y, 0, -1, 420, &cT);
    int eB = findMoatEdge(pt.x, pt.y, 0, +1, 420, &cB);

    if (eL > 0 && eR > eL + 24 && eT > 0 && eB > eT + 18) {
        // 校验外侧护城河是否至少 80% 为同一面板底色（排除普通文字行间空白误判）
        uint32_t candidates[2] = {cL, cT};
        for (int ci = 0; ci < 2; ++ci) {
            uint32_t bg = candidates[ci];
            if (ci == 1 && PixDiff(cL, cT) <= 5) break;
            int ox0 = std::max<int>(subPane.left + 1, eL - 2);
            int ox1 = std::min<int>(subPane.right - 2, eR + 2);
            int oy0 = std::max<int>(subPane.top + 1, eT - 2);
            int oy1 = std::min<int>(subPane.bottom - 2, eB + 2);
            const int M = 16;
            int match = 0, total = M * 4;
            for (int i = 0; i < M; ++i) {
                int sx = ox0 + ((ox1 - ox0) * i) / (M - 1);
                int sy = oy0 + ((oy1 - oy0) * i) / (M - 1);
                if (PixDiff(FastPx(bm, sx, oy0), bg) <= 7) ++match;
                if (PixDiff(FastPx(bm, sx, oy1), bg) <= 7) ++match;
                if (PixDiff(FastPx(bm, ox0, sy), bg) <= 7) ++match;
                if (PixDiff(FastPx(bm, ox1, sy), bg) <= 7) ++match;
            }
            // 并且卡片内部必须存在与底色明显不同的内容像素
            int innerDiff = 0;
            for (int i = 1; i <= 7; ++i) {
                int ix = eL + ((eR - eL) * i) / 8;
                int iy = (eT + eB) / 2;
                if (PixDiff(FastPx(bm, ix, iy), bg) >= 8) ++innerDiff;
            }
            if (match * 100 >= total * 80 && innerDiff >= 2) {
                RECT card{eL + 1, eT + 1, eR, eB};
                if (card.right - card.left >= 24 && card.bottom - card.top >= 18 &&
                    !NearEqualRect(card, subPane, 6)) {
                    AppendUniqueRect(out, card, 4);
                }
                break;
            }
        }
    }
}

static void SnapshotVisibleWindows(OverlayState* st) {
    if (!st) return;
    st->topWins.clear();
    st->monRects.clear();
    EnumWinCtx ctx{st->vs, &st->topWins, &st->monRects};
    EnumWindows(CollectWindowProc, (LPARAM)&ctx);
    EnumDisplayMonitors(NULL, NULL, CollectMonitorProc, (LPARAM)&ctx);
    ZLog(L"[ov] window snapshot topWins=%d mons=%d", (int)st->topWins.size(),
         (int)st->monRects.size());
}

static bool UpdateHoverWindow(OverlayState* st, POINT ptClient, bool preserveWheelLevel = true) {
    if (!st) return false;

    std::vector<RECT> cands;
    // 1) 按 Z 序找到光标命中的最顶层窗口（遮挡在下方的后台窗口绝不穿透识别！）
    for (auto& tw : st->topWins) {
        if (PtInRect(&tw.rcClient, ptClient)) {
            if (!tw.uiaQueued && st->uiaState && st->hwnd) {
                QueueUiaForWindow(st, tw);
            }
            for (const RECT& sr : tw.subRects) {
                if (PtInRect(&sr, ptClient)) cands.push_back(sr);
            }
            DetectVisualRegionsAt(st->shot, tw.rcClient, ptClient, cands);
            cands.push_back(tw.rcClient);
            break;
        }
    }
    // 2) 加上当前所在显示器全屏矩形作为最大层级兜底
    for (const RECT& mr : st->monRects) {
        if (PtInRect(&mr, ptClient)) {
            cands.push_back(mr);
            break;
        }
    }

    // 3) 按面积从小到大排序，过滤几乎完全重合的层级
    auto areaOf = [](const RECT& r) -> long long {
        return (long long)(r.right - r.left) * (long long)(r.bottom - r.top);
    };
    std::sort(cands.begin(), cands.end(),
              [&](const RECT& a, const RECT& b) { return areaOf(a) < areaOf(b); });

    std::vector<RECT> stack;
    for (const RECT& r : cands) {
        if (r.right - r.left < 16 || r.bottom - r.top < 14) continue;
        bool dup = false;
        long long ar = areaOf(r);
        for (const RECT& kept : stack) {
            if (NearEqualRect(kept, r, 6)) {
                dup = true;
                break;
            }
            long long ak = areaOf(kept);
            if (ak > 0 && ar >= ak && (ar - ak) * 100 < ak * 6 &&
                std::abs(r.left - kept.left) <= 10 && std::abs(r.top - kept.top) <= 10) {
                dup = true;
                break;
            }
        }
        if (!dup) stack.push_back(r);
    }

    st->hoverStack = std::move(stack);
    bool ok = !st->hoverStack.empty();
    RECT found{};
    if (ok) {
        if (!preserveWheelLevel || !NearEqualRect(st->hoverStack[0], st->lastSmallestElem, 6)) {
            st->hoverLevel = 0;
            st->lastSmallestElem = st->hoverStack[0];
        }
        if (st->hoverLevel < 0) st->hoverLevel = 0;
        if (st->hoverLevel >= (int)st->hoverStack.size())
            st->hoverLevel = (int)st->hoverStack.size() - 1;
        found = st->hoverStack[st->hoverLevel];
    } else {
        st->hoverLevel = 0;
        st->lastSmallestElem = RECT{};
    }

    if (ok == st->hasHoverWin && (!ok || EqualRect(&found, &st->hoverWin))) return false;
    st->hasHoverWin = ok;
    st->hoverWin = found;
    return true;
}

// ---------------------------------------------------------------- 脏矩形工具

static RECT DecorExpandedRect(RECT r) {
    RECT out{r.left - 12, r.top - 40, r.right + 12, r.bottom + 12};
    if (out.right < out.left + 430) out.right = out.left + 430;
    if (g_ov && out.right > g_ov->dim.w && out.left > g_ov->dim.w - 320) {
        out.left = g_ov->dim.w - 320;
    }
    return out;
}

static void InvalidateClientRect(HWND hwnd, RECT r) {
    if (!g_ov) return;
    RECT full{0, 0, g_ov->dim.w, g_ov->dim.h};
    RECT cl{};
    if (IntersectRect(&cl, &r, &full)) InvalidateRect(hwnd, &cl, FALSE);
}

RECT ComputeHoverTooltipRect(OverlayState* st);

static void InvalidateToolbarArea(HWND hwnd) {
    RECT bar = ToolbarRect();
    if (bar.right > bar.left && bar.bottom > bar.top) {
        InflateRect(&bar, 3, 3);
        InvalidateClientRect(hwnd, bar);
    }
    if (g_ov && g_ov->subBarRect.right > g_ov->subBarRect.left &&
        g_ov->subBarRect.bottom > g_ov->subBarRect.top) {
        RECT sub = g_ov->subBarRect;
        InflateRect(&sub, 3, 3);
        InvalidateClientRect(hwnd, sub);
    }
    if (g_ov) {
        RECT oldTip = g_ov->tipRect;
        g_ov->tipRect = ComputeHoverTooltipRect(g_ov);
        if (oldTip.right > oldTip.left && oldTip.bottom > oldTip.top) {
            InflateRect(&oldTip, 4, 4);
            InvalidateClientRect(hwnd, oldTip);
        }
        if (g_ov->tipRect.right > g_ov->tipRect.left && g_ov->tipRect.bottom > g_ov->tipRect.top) {
            RECT nt = g_ov->tipRect;
            InflateRect(&nt, 4, 4);
            InvalidateClientRect(hwnd, nt);
        }
    }
}

static void InvalidateTwoDecorRects(HWND hwnd, bool hasA, RECT a, bool hasB, RECT b) {
    RECT dirty{};
    if (hasA && hasB) {
        RECT ea = DecorExpandedRect(a);
        RECT eb = DecorExpandedRect(b);
        UnionRect(&dirty, &ea, &eb);
    } else if (hasA) {
        dirty = DecorExpandedRect(a);
    } else if (hasB) {
        dirty = DecorExpandedRect(b);
    } else {
        return;
    }
    InvalidateClientRect(hwnd, dirty);
}

// ---------------------------------------------------------------- 选区命中（平移/8方向缩放）

static int HitTestSelection(const RECT& sel, POINT pt) {
    if (sel.right - sel.left < 4 || sel.bottom - sel.top < 4) return HIT_NONE;
    const int p = 7;
    bool nearL = std::abs(pt.x - sel.left) <= p;
    bool nearR = std::abs(pt.x - sel.right) <= p;
    bool nearT = std::abs(pt.y - sel.top) <= p;
    bool nearB = std::abs(pt.y - sel.bottom) <= p;
    bool inX = pt.x >= sel.left - p && pt.x <= sel.right + p;
    bool inY = pt.y >= sel.top - p && pt.y <= sel.bottom + p;

    if (nearL && nearT) return HIT_TL;
    if (nearR && nearT) return HIT_TR;
    if (nearL && nearB) return HIT_BL;
    if (nearR && nearB) return HIT_BR;
    if (nearT && inX) return HIT_T;
    if (nearB && inX) return HIT_B;
    if (nearL && inY) return HIT_L;
    if (nearR && inY) return HIT_R;
    if (PtInRect(&sel, pt)) return HIT_MOVE;
    return HIT_NONE;
}

static LPCWSTR CursorForSelHit(int hit) {
    switch (hit) {
    case HIT_TL:
    case HIT_BR: return IDC_SIZENWSE;
    case HIT_TR:
    case HIT_BL: return IDC_SIZENESW;
    case HIT_L:
    case HIT_R: return IDC_SIZEWE;
    case HIT_T:
    case HIT_B: return IDC_SIZENS;
    case HIT_MOVE: return IDC_SIZEALL;
    default: return IDC_CROSS;
    }
}

// ---------------------------------------------------------------- 取色器颜色格式化

static uint32_t SampleShotPixel(const Bitmap32& shot, int x, int y) {
    if (shot.empty()) return 0xFF000000u;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= shot.w) x = shot.w - 1;
    if (y >= shot.h) y = shot.h - 1;
    return shot.row(y)[x];
}

static std::wstring FormatPickedColor(uint32_t argb, int fmt) {
    int a = (int)((argb >> 24) & 0xFF);
    int r = (int)((argb >> 16) & 0xFF);
    int g = (int)((argb >> 8) & 0xFF);
    int b = (int)(argb & 0xFF);
    wchar_t buf[96] = {};

    switch (fmt) {
    case 0:  // HEX（不含透明度）
        swprintf(buf, 96, L"#%02X%02X%02X", r, g, b);
        return buf;
    case 1:  // HEXA（含透明度）
        swprintf(buf, 96, L"#%02X%02X%02X%02X", r, g, b, a);
        return buf;
    case 2:  // RGB
        swprintf(buf, 96, L"rgb(%d, %d, %d)", r, g, b);
        return buf;
    case 3:  // RGBA
        if (a == 255)
            swprintf(buf, 96, L"rgba(%d, %d, %d, 1)", r, g, b);
        else
            swprintf(buf, 96, L"rgba(%d, %d, %d, %.2f)", r, g, b, a / 255.0);
        return buf;
    case 4:
    case 5: {
        double rf = r / 255.0, gf = g / 255.0, bf = b / 255.0;
        double cmax = std::max(rf, std::max(gf, bf));
        double cmin = std::min(rf, std::min(gf, bf));
        double d = cmax - cmin;
        double l = (cmax + cmin) * 0.5;
        double h = 0.0, s = 0.0;
        if (d > 1e-9) {
            s = (l > 0.5) ? (d / (2.0 - cmax - cmin)) : (d / (cmax + cmin));
            if (cmax == rf) h = (gf - bf) / d + (gf < bf ? 6.0 : 0.0);
            else if (cmax == gf) h = (bf - rf) / d + 2.0;
            else h = (rf - gf) / d + 4.0;
            h *= 60.0;
        }
        int hi = ((int)std::lround(h)) % 360;
        if (hi < 0) hi += 360;
        int si = (int)std::lround(s * 100.0);
        int li = (int)std::lround(l * 100.0);
        if (fmt == 4) {
            swprintf(buf, 96, L"hsl(%d, %d%%, %d%%)", hi, si, li);
        } else if (a == 255) {
            swprintf(buf, 96, L"hsla(%d, %d%%, %d%%, 1)", hi, si, li);
        } else {
            swprintf(buf, 96, L"hsla(%d, %d%%, %d%%, %.2f)", hi, si, li, a / 255.0);
        }
        return buf;
    }
    }
    swprintf(buf, 96, L"#%02X%02X%02X", r, g, b);
    return buf;
}

static RECT ComputeLoupeRect(POINT ptClient) {
    const int lw = 186;
    const int lh = 176;
    if (!g_ov) return RECT{0, 0, lw, lh};
    POINT sp{ptClient.x + g_ov->vs.left, ptClient.y + g_ov->vs.top};
    RECT mon = MonitorWorkRect(sp);
    OffsetRect(&mon, -g_ov->vs.left, -g_ov->vs.top);

    int x = ptClient.x + 18;
    int y = ptClient.y + 18;
    if (x + lw > mon.right - 8) x = ptClient.x - lw - 18;
    if (y + lh > mon.bottom - 8) y = ptClient.y - lh - 18;
    if (x < mon.left + 8) x = mon.left + 8;
    if (y < mon.top + 8) y = mon.top + 8;
    return RECT{x, y, x + lw, y + lh};
}

// ---------------------------------------------------------------- 工具栏 & 二级属性条

RECT ToolbarRect() {
    if (!g_ov) return RECT{0, 0, 0, 0};
    for (auto& b : g_ov->btns)
        if (b.id == 0) return b.rc;
    return RECT{0, 0, 0, 0};
}

int HitTestButton(POINT ptClient) {
    if (!g_ov) return 0;
    for (auto& b : g_ov->btns) {
        if (b.id == 0) continue;
        if (PtInRect(&b.rc, ptClient)) return b.id;
    }
    return 0;
}

int HitTestSubButton(POINT ptClient) {
    if (!g_ov) return 0;
    for (auto& b : g_ov->subBtns) {
        if (PtInRect(&b.rc, ptClient)) return b.id;
    }
    return 0;
}

bool PointInAnyToolbar(POINT ptClient) {
    if (!g_ov || g_ov->phase != 2) return false;
    RECT bar = ToolbarRect();
    if (PtInRect(&bar, ptClient)) return true;
    if (g_ov->subBarRect.right > g_ov->subBarRect.left &&
        PtInRect(&g_ov->subBarRect, ptClient))
        return true;
    return false;
}

void LayoutSubToolbar() {
    if (!g_ov) return;
    g_ov->subBtns.clear();
    g_ov->subBarRect = RECT{};
    g_ov->subSepX = 0;
    if (g_ov->phase != 2 || (g_ov->tool == TOOL_NONE && !g_ov->imgSearchSubOpen)) return;

    RECT bar = ToolbarRect();
    if (bar.right <= bar.left) return;

    const int subH = 38;
    const int padX = 10;
    int curX = padX;
    int nextId = 100;
    bool centerOnSearchBtn = false;

    if (g_ov->tool == TOOL_NONE && g_ov->imgSearchSubOpen) {
        centerOnSearchBtn = true;
        HDC dc = GetDC(g_ov->hwnd);
        HGDIOBJ of = SelectObject(dc, g_ov->fontSmall);
        for (int i = 0; i < IMG_ENGINE_COUNT; ++i) {
            const wchar_t* lbl = img_search::EngineLabel(i);
            SIZE sz{};
            GetTextExtentPoint32W(dc, lbl, (int)wcslen(lbl), &sz);
            int w = sz.cx + 18;
            if (w < 46) w = 46;
            SubBtn sb;
            sb.id = nextId++;
            sb.kind = SUB_IMGENGINE;
            sb.value = i;
            sb.label = lbl;
            sb.rc = RECT{curX, 6, curX + w, subH - 6};
            g_ov->subBtns.push_back(sb);
            curX += w + 4;
        }
        SelectObject(dc, of);
        ReleaseDC(g_ov->hwnd, dc);
        curX += padX - 4;
    } else if (g_ov->tool == TOOL_PICKER) {
        HDC dc = GetDC(g_ov->hwnd);
        HGDIOBJ of = SelectObject(dc, g_ov->fontSmall);
        for (int i = 0; i < 6; ++i) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, kPickerFmtLabels[i], (int)wcslen(kPickerFmtLabels[i]), &sz);
            int w = sz.cx + 18;
            if (w < 44) w = 44;
            SubBtn sb;
            sb.id = nextId++;
            sb.kind = SUB_PICKFMT;
            sb.value = i;
            sb.label = kPickerFmtLabels[i];
            sb.rc = RECT{curX, 6, curX + w, subH - 6};
            g_ov->subBtns.push_back(sb);
            curX += w + 4;
        }
        SelectObject(dc, of);
        ReleaseDC(g_ov->hwnd, dc);
        curX += padX - 4;
    } else if (g_ov->tool == TOOL_MOSAIC) {
        const wchar_t* labels[3] = {L"细", L"中", L"粗"};
        for (int i = 0; i < 3; ++i) {
            SubBtn sb;
            sb.id = nextId++;
            sb.kind = SUB_SIZE;
            sb.value = i;
            sb.label = labels[i];
            sb.rc = RECT{curX, 6, curX + 34, subH - 6};
            g_ov->subBtns.push_back(sb);
            curX += 38;
        }
        curX += padX - 4;
    } else {
        // 箭头 / 矩形 / 文本：左侧 3 档大小 + 分隔线 + 右侧 6 种常用颜色
        const wchar_t* textLabels[3] = {L"16", L"22", L"32"};
        for (int i = 0; i < 3; ++i) {
            SubBtn sb;
            sb.id = nextId++;
            sb.kind = SUB_SIZE;
            sb.value = i;
            if (g_ov->tool == TOOL_TEXT) sb.label = textLabels[i];
            int w = (g_ov->tool == TOOL_TEXT) ? 34 : 28;
            sb.rc = RECT{curX, 6, curX + w, subH - 6};
            g_ov->subBtns.push_back(sb);
            curX += w + 4;
        }
        curX += 4;
        g_ov->subSepX = curX;
        curX += 10;

        for (int i = 0; i < 6; ++i) {
            SubBtn sb;
            sb.id = nextId++;
            sb.kind = SUB_COLOR;
            sb.value = i;
            sb.color = kPalette[i];
            sb.rc = RECT{curX, 7, curX + 24, subH - 7};
            g_ov->subBtns.push_back(sb);
            curX += 28;
        }
        curX += padX - 4;
    }

    int totalW = curX;
    POINT selCenterScreen{(g_ov->sel.left + g_ov->sel.right) / 2 + g_ov->vs.left,
                          (g_ov->sel.top + g_ov->sel.bottom) / 2 + g_ov->vs.top};
    RECT mon = MonitorWorkRect(selCenterScreen);
    OffsetRect(&mon, -g_ov->vs.left, -g_ov->vs.top);

    // 默认贴在主工具栏下方，靠近底部则贴到主工具栏上方
    int sx = bar.left;
    if (centerOnSearchBtn) {
        for (const auto& b : g_ov->btns) {
            if (b.id == BTN_IMG_SEARCH) {
                sx = (b.rc.left + b.rc.right - totalW) / 2;
                break;
            }
        }
    }
    int sy = bar.bottom + 6;
    if (sy + subH > mon.bottom - 8) sy = bar.top - subH - 6;
    if (sy < mon.top + 8) sy = bar.bottom + 6;
    if (sx + totalW > mon.right - 8) sx = mon.right - totalW - 8;
    if (sx < mon.left + 8) sx = mon.left + 8;

    g_ov->subBarRect = RECT{sx, sy, sx + totalW, sy + subH};
    if (g_ov->subSepX > 0) g_ov->subSepX += sx;
    for (auto& sb : g_ov->subBtns) {
        OffsetRect(&sb.rc, sx, sy);
    }
}

void LayoutToolbar() {
    if (!g_ov) return;
    g_ov->btns.clear();
    g_ov->barSepXs.clear();
    g_ov->subBtns.clear();
    g_ov->subBarRect = RECT{};
    if (g_ov->phase != 2) {
        g_ov->tipRect = RECT{};
        return;
    }

    struct Def {
        int id;
        int iconIdx;
        const wchar_t* label;
        std::wstring key;
    };
    auto selCoveredByCache = [&]() {
        if (!g_ov->imgTrHasCache) return false;
        return g_ov->sel.left >= g_ov->imgTrSel.left - 4 &&
               g_ov->sel.top >= g_ov->imgTrSel.top - 4 &&
               g_ov->sel.right <= g_ov->imgTrSel.right + 4 &&
               g_ov->sel.bottom <= g_ov->imgTrSel.bottom + 4;
    };
    const wchar_t* imgTrTip =
        g_ov->imgTrBusy
            ? L"正在图内翻译…"
            : (g_ov->imgTrHasCache
                   ? (!selCoveredByCache() ? L"重新图内翻译"
                                           : (g_ov->imgTrActive ? L"切换原图" : L"显示译文"))
                   : L"图内翻译");
    std::wstring searchTip = std::wstring(L"识图 (") +
                             img_search::EngineLabel(g_cfg.imageSearchEngine) + L")";
    // 最左：取色器 | 标注组（马赛克/输入文本/矩形/箭头）| 动作组（识图/图内翻译/翻译/提取文本/下载/贴图/复制）
    std::vector<Def> defs = {
        {BTN_PICKER, toolbar_icons::ICON_PICKER, L"取色", L"C"},
        {BTN_MOSAIC, toolbar_icons::ICON_MOSAIC, L"马赛克", L"M"},
        {BTN_TEXT, toolbar_icons::ICON_TEXT, L"输入文本", L"T"},
        {BTN_RECT, toolbar_icons::ICON_RECT, L"矩形", L"R"},
        {BTN_ARROW, toolbar_icons::ICON_ARROW, L"箭头", L"A"},
        {BTN_IMG_SEARCH, toolbar_icons::ICON_IMG_SEARCH, searchTip.c_str(), L"S"},
        {BTN_IMG_TRANSLATE, toolbar_icons::ICON_IMG_TRANSLATE, imgTrTip, L"G"},
        {BTN_TRANSLATE, toolbar_icons::ICON_TRANSLATE, L"翻译", g_cfg.translate},
        {BTN_OCR, toolbar_icons::ICON_OCR, L"提取文本", g_cfg.copyText},
        {BTN_SAVE, toolbar_icons::ICON_SAVE, L"下载", L"Ctrl+S"},
        {BTN_PIN, toolbar_icons::ICON_PIN, L"贴图", g_cfg.pin},
        {BTN_COPY, toolbar_icons::ICON_COPY, L"复制", g_cfg.copyImage},
    };

    const int btnSz = 36;
    const int gap = 3;
    const int sepW = 11;
    const int barPad = 5;

    int total = barPad * 2 + (int)defs.size() * btnSz + ((int)defs.size() - 1) * gap + 2 * sepW;
    int barH = btnSz + barPad * 2;

    POINT selCenterScreen{(g_ov->sel.left + g_ov->sel.right) / 2 + g_ov->vs.left,
                          (g_ov->sel.top + g_ov->sel.bottom) / 2 + g_ov->vs.top};
    RECT monClient = MonitorWorkRect(selCenterScreen);
    OffsetRect(&monClient, -g_ov->vs.left, -g_ov->vs.top);

    // 选区较宽时右对齐，截小图时自动水平居中对齐选区，视觉更均衡
    int selW = g_ov->sel.right - g_ov->sel.left;
    int x = (selW >= total) ? (g_ov->sel.right - total)
                            : ((g_ov->sel.left + g_ov->sel.right - total) / 2);
    int y = g_ov->sel.bottom + 10;
    if (y + barH > monClient.bottom - 8) y = g_ov->sel.top - barH - 10;
    if (y < monClient.top + 8) y = monClient.bottom - barH - 8;
    if (x < monClient.left + 8) x = monClient.left + 8;
    if (x + total > monClient.right - 8) x = monClient.right - total - 8;

    int curX = x + barPad;
    for (size_t i = 0; i < defs.size(); ++i) {
        if (i > 0 && (defs[i].id == BTN_MOSAIC || defs[i].id == BTN_IMG_SEARCH)) {
            g_ov->barSepXs.push_back(curX + sepW / 2 - 1);
            curX += sepW;
        }
        Btn b;
        b.id = defs[i].id;
        b.iconIdx = defs[i].iconIdx;
        b.label = defs[i].label;
        b.key = defs[i].key;
        b.rc.left = curX;
        b.rc.top = y + barPad;
        b.rc.right = curX + btnSz;
        b.rc.bottom = y + barPad + btnSz;
        g_ov->btns.push_back(b);
        curX += btnSz + gap;
    }

    if (!g_ov->btns.empty()) {
        RECT first = g_ov->btns.front().rc;
        RECT last = g_ov->btns.back().rc;
        Btn bar;
        bar.id = 0;
        bar.rc = RECT{first.left - barPad, first.top - barPad, last.right + barPad,
                      last.bottom + barPad};
        g_ov->btns.push_back(bar);
    }

    LayoutSubToolbar();
    g_ov->tipRect = ComputeHoverTooltipRect(g_ov);
}

// 48x48 超采样面积积分缩放缓存 + 预乘 AlphaBlend 渲染统一配色图标
static void DrawToolbarIcon(HDC dc, int iconIdx, RECT btnRc, COLORREF color) {
    if (iconIdx < 0 || iconIdx >= toolbar_icons::ICON_COUNT) return;
    constexpr int kDst = 20;
    static uint8_t s_mask20[toolbar_icons::ICON_COUNT][kDst * kDst];
    static bool s_ready = false;
    if (!s_ready) {
        const double ratio = (double)toolbar_icons::kIconGrid / (double)kDst;
        for (int ic = 0; ic < toolbar_icons::ICON_COUNT; ++ic) {
            const uint8_t* src = toolbar_icons::kAlpha[ic];
            for (int dy = 0; dy < kDst; ++dy) {
                double sy0 = dy * ratio, sy1 = (dy + 1) * ratio;
                int iy0 = (int)std::floor(sy0);
                int iy1 = std::min(toolbar_icons::kIconGrid - 1, (int)std::ceil(sy1) - 1);
                for (int dx = 0; dx < kDst; ++dx) {
                    double sx0 = dx * ratio, sx1 = (dx + 1) * ratio;
                    int ix0 = (int)std::floor(sx0);
                    int ix1 = std::min(toolbar_icons::kIconGrid - 1, (int)std::ceil(sx1) - 1);
                    double acc = 0.0, wsum = 0.0;
                    for (int sy = iy0; sy <= iy1; ++sy) {
                        double wy = std::min(sy + 1.0, sy1) - std::max((double)sy, sy0);
                        if (wy <= 0.0) continue;
                        for (int sx = ix0; sx <= ix1; ++sx) {
                            double wx = std::min(sx + 1.0, sx1) - std::max((double)sx, sx0);
                            if (wx <= 0.0) continue;
                            double w = wx * wy;
                            acc += src[sy * toolbar_icons::kIconGrid + sx] * w;
                            wsum += w;
                        }
                    }
                    double val = (wsum > 0.0) ? (acc / wsum) : 0.0;
                    double nrm = std::pow(std::min(1.0, std::max(0.0, val / 255.0)), 0.88);
                    s_mask20[ic][dy * kDst + dx] = (uint8_t)std::lround(nrm * 255.0);
                }
            }
        }
        s_ready = true;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kDst;
    bi.bmiHeader.biHeight = -kDst;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (mem && dib && bits) {
        uint32_t* px = (uint32_t*)bits;
        const uint8_t* mask = s_mask20[iconIdx];
        int cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
        for (int i = 0; i < kDst * kDst; ++i) {
            uint32_t a = mask[i];
            uint32_t pr = (cr * a + 127) / 255;
            uint32_t pg = (cg * a + 127) / 255;
            uint32_t pb = (cb * a + 127) / 255;
            px[i] = (a << 24) | (pr << 16) | (pg << 8) | pb;
        }
        HGDIOBJ old = SelectObject(mem, dib);
        int ix = btnRc.left + ((btnRc.right - btnRc.left) - kDst) / 2;
        int iy = btnRc.top + ((btnRc.bottom - btnRc.top) - kDst) / 2;
        BLENDFUNCTION bf{};
        bf.BlendOp = AC_SRC_OVER;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        AlphaBlend(dc, ix, iy, kDst, kDst, mem, 0, 0, kDst, kDst, bf);
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
}

RECT ComputeHoverTooltipRect(OverlayState* st) {
    if (!st || st->phase != 2 || st->hover == 0 || !st->hwnd) return RECT{};
    const Btn* hb = nullptr;
    for (const auto& b : st->btns) {
        if (b.id == st->hover) {
            hb = &b;
            break;
        }
    }
    if (!hb || hb->label.empty()) return RECT{};

    HDC dc = GetDC(st->hwnd);
    HGDIOBJ of = SelectObject(dc, st->fontBtn);
    SIZE sLbl{}, sKey{};
    GetTextExtentPoint32W(dc, hb->label.c_str(), (int)hb->label.size(), &sLbl);
    if (!hb->key.empty()) {
        SelectObject(dc, st->fontSmall);
        GetTextExtentPoint32W(dc, hb->key.c_str(), (int)hb->key.size(), &sKey);
    }
    SelectObject(dc, of);
    ReleaseDC(st->hwnd, dc);

    int keyPillW = hb->key.empty() ? 0 : (sKey.cx + 12);
    int tipW = 10 + sLbl.cx + (keyPillW > 0 ? (7 + keyPillW) : 0) + 10;
    int tipH = 28;

    RECT bar = ToolbarRect();
    POINT selCenterScreen{(st->sel.left + st->sel.right) / 2 + st->vs.left,
                          (st->sel.top + st->sel.bottom) / 2 + st->vs.top};
    RECT monClient = MonitorWorkRect(selCenterScreen);
    OffsetRect(&monClient, -st->vs.left, -st->vs.top);

    int tx = (hb->rc.left + hb->rc.right - tipW) / 2;
    if (tx < monClient.left + 6) tx = monClient.left + 6;
    if (tx + tipW > monClient.right - 6) tx = monClient.right - tipW - 6;

    // 默认放在工具栏正下方（不遮挡上方截图内容）；若二级属性条也在下方且水平重叠，则顺延到二级条下方
    int ty = bar.bottom + 6;
    const RECT& sr = st->subBarRect;
    if (sr.right > sr.left && sr.top >= bar.bottom - 2) {
        if (tx + tipW > sr.left - 4 && tx < sr.right + 4) {
            ty = sr.bottom + 6;
        }
    }
    if (ty + tipH > monClient.bottom - 6) {
        int topRef = bar.top;
        if (sr.right > sr.left && sr.bottom <= bar.top + 2 && tx + tipW > sr.left - 4 &&
            tx < sr.right + 4) {
            topRef = sr.top;
        }
        ty = topRef - tipH - 6;
    }
    return RECT{tx, ty, tx + tipW, ty + tipH};
}

static void DrawHoverTooltip(HDC dc) {
    if (!g_ov || g_ov->phase != 2 || g_ov->hover == 0) return;
    RECT tr = g_ov->tipRect;
    if (tr.right <= tr.left || tr.bottom <= tr.top) return;

    const Btn* hb = nullptr;
    for (const auto& b : g_ov->btns) {
        if (b.id == g_ov->hover) {
            hb = &b;
            break;
        }
    }
    if (!hb) return;

    ui::RoundRect(dc, tr, 6, RGB(28, 30, 35), RGB(68, 72, 82));

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_ov->fontBtn);
    SIZE sLbl{};
    GetTextExtentPoint32W(dc, hb->label.c_str(), (int)hb->label.size(), &sLbl);

    RECT lrc{tr.left + 10, tr.top, tr.left + 10 + sLbl.cx + 2, tr.bottom};
    SetTextColor(dc, RGB(246, 247, 249));
    DrawTextW(dc, hb->label.c_str(), -1, &lrc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (!hb->key.empty()) {
        SelectObject(dc, g_ov->fontSmall);
        SIZE sKey{};
        GetTextExtentPoint32W(dc, hb->key.c_str(), (int)hb->key.size(), &sKey);
        int kw = sKey.cx + 12;
        int kx = tr.left + 10 + sLbl.cx + 7;
        RECT krc{kx, tr.top + 5, kx + kw, tr.bottom - 5};
        ui::RoundRect(dc, krc, 4, RGB(54, 58, 68), CLR_INVALID);
        SetTextColor(dc, RGB(246, 170, 56));
        DrawTextW(dc, hb->key.c_str(), -1, &krc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
}

void DrawSubToolbar(HDC dc) {
    if (!g_ov || g_ov->subBtns.empty()) return;
    RECT sr = g_ov->subBarRect;
    ui::RoundRect(dc, sr, 9, kBarBg, kBarBorder);

    if (g_ov->subSepX > 0) {
        RECT sep{g_ov->subSepX, sr.top + 9, g_ov->subSepX + 1, sr.bottom - 9};
        HBRUSH hb = CreateSolidBrush(kBarSep);
        FillRect(dc, &sep, hb);
        DeleteObject(hb);
    }

    for (const auto& sb : g_ov->subBtns) {
        bool hov = (sb.id == g_ov->hoverSub || sb.id == g_ov->pressedSub);
        if (sb.kind == SUB_COLOR) {
            bool active = (g_ov->colorIdx == sb.value);
            if (active || hov) {
                ui::RoundRect(dc, sb.rc, 6, active ? kBarActiveBg : kBarHover,
                              active ? kBarAccent : CLR_INVALID);
            }
            RECT dot = sb.rc;
            InflateRect(&dot, -4, -4);
            COLORREF border = (sb.color == RGB(255, 255, 255)) ? RGB(180, 184, 192) : CLR_INVALID;
            ui::RoundRect(dc, dot, (dot.right - dot.left) / 2, sb.color, border);
        } else if (sb.kind == SUB_SIZE) {
            bool active = false;
            if (g_ov->tool == TOOL_TEXT) active = (g_ov->textSizeIdx == sb.value);
            else if (g_ov->tool == TOOL_MOSAIC) active = (g_ov->mosaicIdx == sb.value);
            else active = (g_ov->strokeIdx == sb.value);

            COLORREF bg = active ? kBarActiveBg : (hov ? kBarHover : kBarBtnBg);
            ui::RoundRect(dc, sb.rc, 6, bg, active ? kBarAccent : CLR_INVALID);

            if (!sb.label.empty()) {
                ui::Text(dc, sb.label, sb.rc, active ? kBarAccentDark : kBarText, g_ov->fontSmall,
                         DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            } else {
                // 线条粗细圆点
                int d = (sb.value == 0) ? 6 : (sb.value == 1 ? 10 : 14);
                int cx = (sb.rc.left + sb.rc.right) / 2;
                int cy = (sb.rc.top + sb.rc.bottom) / 2;
                RECT dot{cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d};
                ui::RoundRect(dc, dot, d / 2, active ? kBarAccent : kBarText, CLR_INVALID);
            }
        } else if (sb.kind == SUB_PICKFMT) {
            bool active = (g_ov->pickerFmt == sb.value);
            COLORREF bg = active ? kBarActiveBg : (hov ? kBarHover : kBarBtnBg);
            ui::RoundRect(dc, sb.rc, 6, bg, active ? kBarAccent : CLR_INVALID);
            ui::Text(dc, sb.label, sb.rc, active ? kBarAccentDark : kBarText, g_ov->fontSmall,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else if (sb.kind == SUB_IMGENGINE) {
            bool active = (g_cfg.imageSearchEngine == sb.value);
            COLORREF bg = active ? kBarActiveBg : (hov ? kBarHover : kBarBtnBg);
            ui::RoundRect(dc, sb.rc, 6, bg, active ? kBarAccent : CLR_INVALID);
            ui::Text(dc, sb.label, sb.rc, active ? kBarAccentDark : kBarText, g_ov->fontSmall,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
}

void DrawToolbar(HDC dc) {
    if (!g_ov || g_ov->phase != 2 || g_ov->btns.empty()) return;
    RECT bar = ToolbarRect();
    ui::RoundRect(dc, bar, 9, kBarBg, kBarBorder);

    for (int sx : g_ov->barSepXs) {
        RECT sep{sx, bar.top + 10, sx + 1, bar.bottom - 10};
        HBRUSH hb = CreateSolidBrush(kBarSep);
        FillRect(dc, &sep, hb);
        DeleteObject(hb);
    }

    // 统一图标配色：常规态柔和深石板色、悬浮态深墨色、激活态品牌橙色
    const COLORREF kIconNormal = RGB(48, 53, 62);
    const COLORREF kIconHover = RGB(20, 22, 26);
    const COLORREF kIconActive = RGB(220, 120, 8);

    for (auto& b : g_ov->btns) {
        if (b.id == 0) continue;
        bool active = (b.id == BTN_ARROW && g_ov->tool == TOOL_ARROW) ||
                      (b.id == BTN_RECT && g_ov->tool == TOOL_RECT) ||
                      (b.id == BTN_TEXT && g_ov->tool == TOOL_TEXT) ||
                      (b.id == BTN_MOSAIC && g_ov->tool == TOOL_MOSAIC) ||
                      (b.id == BTN_PICKER && g_ov->tool == TOOL_PICKER) ||
                      (b.id == BTN_IMG_TRANSLATE && (g_ov->imgTrActive || g_ov->imgTrBusy));
        bool hov = (b.id == g_ov->hover || b.id == g_ov->pressed);
        COLORREF bg = kBarBtnBg;
        if (active)
            bg = kBarActiveBg;
        else if (hov)
            bg = kBarHover;
        ui::RoundRect(dc, b.rc, 6, bg, active ? kBarAccent : CLR_INVALID);

        COLORREF iconCol = active ? kIconActive : (hov ? kIconHover : kIconNormal);
        DrawToolbarIcon(dc, b.iconIdx, b.rc, iconCol);
    }

    DrawSubToolbar(dc);
    DrawHoverTooltip(dc);
}

// ---------------------------------------------------------------- 取色放大镜绘制

void DrawColorLoupe(HDC dc) {
    if (!g_ov || g_ov->phase != 2 || g_ov->tool != TOOL_PICKER || !g_ov->hasMousePt) return;
    if (PointInAnyToolbar(g_ov->mousePt)) return;

    RECT lr = g_ov->loupeRect;
    if (lr.right <= lr.left) return;

    const int cell = 12;
    const int cols = 15;
    const int rows = 9;
    const int gridW = cols * cell;  // 180
    const int gridH = rows * cell;  // 108

    // 外框底卡片
    ui::RoundRect(dc, lr, 8, RGB(26, 27, 30), RGB(76, 80, 88));

    int gx = lr.left + 3;
    int gy = lr.top + 3;
    int cx = g_ov->mousePt.x;
    int cy = g_ov->mousePt.y;

    // 15×9 放大像素格（最近邻锐利采样）
    for (int ry = 0; ry < rows; ++ry) {
        int sy = cy - rows / 2 + ry;
        for (int rx = 0; rx < cols; ++rx) {
            int sx = cx - cols / 2 + rx;
            COLORREF col = RGB(24, 24, 28);
            if (sx >= 0 && sy >= 0 && sx < g_ov->shot.w && sy < g_ov->shot.h) {
                uint32_t px = g_ov->shot.row(sy)[sx];
                col = RGB((px >> 16) & 0xFF, (px >> 8) & 0xFF, px & 0xFF);
            }
            RECT cr{gx + rx * cell, gy + ry * cell, gx + (rx + 1) * cell, gy + (ry + 1) * cell};
            HBRUSH br = CreateSolidBrush(col);
            FillRect(dc, &cr, br);
            DeleteObject(br);
        }
    }

    // 细网格线
    HPEN gpen = CreatePen(PS_SOLID, 1, RGB(60, 62, 68));
    HGDIOBJ op = SelectObject(dc, gpen);
    for (int i = 1; i < cols; ++i) {
        MoveToEx(dc, gx + i * cell, gy, NULL);
        LineTo(dc, gx + i * cell, gy + gridH);
    }
    for (int j = 1; j < rows; ++j) {
        MoveToEx(dc, gx, gy + j * cell, NULL);
        LineTo(dc, gx + gridW, gy + j * cell);
    }
    SelectObject(dc, op);
    DeleteObject(gpen);

    // 中心目标像素双层准星（黑+白，明暗背景均清晰）
    RECT mid{gx + (cols / 2) * cell, gy + (rows / 2) * cell, gx + (cols / 2 + 1) * cell,
             gy + (rows / 2 + 1) * cell};
    {
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        HPEN p1 = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        HGDIOBJ oldP = SelectObject(dc, p1);
        Rectangle(dc, mid.left - 1, mid.top - 1, mid.right + 1, mid.bottom + 1);
        HPEN p2 = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
        SelectObject(dc, p2);
        Rectangle(dc, mid.left, mid.top, mid.right, mid.bottom);
        SelectObject(dc, oldP);
        SelectObject(dc, ob);
        DeleteObject(p1);
        DeleteObject(p2);
    }

    // 底部信息区：色块 + 格式化色值 + 提示
    uint32_t centerPx = SampleShotPixel(g_ov->shot, cx, cy);
    COLORREF centerCol = RGB((centerPx >> 16) & 0xFF, (centerPx >> 8) & 0xFF, centerPx & 0xFF);
    std::wstring val = FormatPickedColor(centerPx, g_ov->pickerFmt);

    int infoY = gy + gridH + 8;
    RECT sw{lr.left + 10, infoY + 2, lr.left + 28, infoY + 20};
    ui::RoundRect(dc, sw, 4, centerCol, RGB(120, 124, 132));

    RECT vr{lr.left + 35, infoY, lr.right - 8, infoY + 22};
    ui::Text(dc, val, vr, RGB(245, 246, 248), g_ov->fontDim,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    wchar_t sub[96];
    swprintf(sub, 96, L"(%d, %d) · 单击复制 · Tab 切格式", cx + (int)g_ov->vs.left,
             cy + (int)g_ov->vs.top);
    RECT sr{lr.left + 10, infoY + 26, lr.right - 8, infoY + 46};
    ui::Text(dc, sub, sr, RGB(158, 162, 170), g_ov->fontKey,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// ---------------------------------------------------------------- 透明背景实时文字编辑

static RECT ComputeEditVisualRect() {
    if (!g_ov) return RECT{};
    int fsz = kTextSizes[g_ov->textSizeIdx];
    int x = g_ov->editPosScreen.x - g_ov->vs.left;
    int y = g_ov->editPosScreen.y - g_ov->vs.top;
    std::wstring disp = g_ov->editText.empty() ? L"输入文字…" : g_ov->editText;
    HDC dc = GetDC(g_ov->hwnd);
    HFONT font = MakeUiFont(fsz, true);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, disp.c_str(), (int)disp.size(), &sz);
    SelectObject(dc, of);
    DeleteObject(font);
    ReleaseDC(g_ov->hwnd, dc);
    int w = std::max((int)sz.cx + 22, 110);
    int h = std::max((int)sz.cy + 12, fsz + 14);
    return RECT{x - 4, y - 4, x + w, y + h};
}

static void SyncEditFromControl() {
    if (!g_ov || !g_ov->hEdit) return;
    RECT oldRc = g_ov->editVisualRect;
    wchar_t buf[1024] = {};
    GetWindowTextW(g_ov->hEdit, buf, 1024);
    g_ov->editText = buf;
    DWORD s0 = 0, s1 = 0;
    SendMessageW(g_ov->hEdit, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    g_ov->editCaret = (int)s1;
    if (g_ov->editCaret > (int)g_ov->editText.size())
        g_ov->editCaret = (int)g_ov->editText.size();
    g_ov->editCaretOn = true;
    g_ov->editVisualRect = ComputeEditVisualRect();

    // 让不可见 EDIT 跟随光标移动，中文输入法候选框始终贴在当前输入位置下方
    int fsz = kTextSizes[g_ov->textSizeIdx];
    int x = g_ov->editPosScreen.x - g_ov->vs.left;
    int y = g_ov->editPosScreen.y - g_ov->vs.top;
    int caretDx = 0;
    if (g_ov->editCaret > 0) {
        HDC dc = GetDC(g_ov->hwnd);
        HFONT font = MakeUiFont(fsz, true);
        HGDIOBJ of = SelectObject(dc, font);
        std::wstring prefix = g_ov->editText.substr(0, (size_t)g_ov->editCaret);
        SIZE sz{};
        GetTextExtentPoint32W(dc, prefix.c_str(), (int)prefix.size(), &sz);
        caretDx = sz.cx;
        SelectObject(dc, of);
        DeleteObject(font);
        ReleaseDC(g_ov->hwnd, dc);
    }
    SetWindowPos(g_ov->hEdit, NULL, x + caretDx, y + fsz, 1, 1, SWP_NOZORDER | SWP_NOACTIVATE);

    RECT dirty{};
    UnionRect(&dirty, &oldRc, &g_ov->editVisualRect);
    InflateRect(&dirty, 8, 8);
    InvalidateClientRect(g_ov->hwnd, dirty);
}

static void DrawLiveEdit(HDC dc) {
    if (!g_ov || !g_ov->hEdit) return;
    int fsz = kTextSizes[g_ov->textSizeIdx];
    COLORREF col = kPalette[g_ov->colorIdx];
    int x = g_ov->editPosScreen.x - g_ov->vs.left;
    int y = g_ov->editPosScreen.y - g_ov->vs.top;
    RECT box = g_ov->editVisualRect;

    // 透明背景 + 虚线细框（无白底！）
    HPEN pen = CreatePen(PS_DASH, 1, kBarAccent);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    Rectangle(dc, box.left, box.top, box.right, box.bottom);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);

    HFONT font = MakeUiFont(fsz, true);
    HGDIOBJ of = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    if (g_ov->editText.empty()) {
        RECT pr{x, y, box.right, box.bottom};
        SetTextColor(dc, RGB(220, 224, 230));
        DrawTextW(dc, L"输入文字…", -1, &pr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        RECT tr{x, y, x + 1200, y + 200};
        int lum = (GetRValue(col) * 299 + GetGValue(col) * 587 + GetBValue(col) * 114) / 1000;
        COLORREF shadowCol = (lum < 60) ? RGB(240, 240, 245) : RGB(20, 20, 24);
        RECT sh = tr;
        sh.left += 1;
        sh.top += 1;
        SetTextColor(dc, shadowCol);
        DrawTextW(dc, g_ov->editText.c_str(), -1, &sh,
                  DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(dc, col);
        DrawTextW(dc, g_ov->editText.c_str(), -1, &tr,
                  DT_LEFT | DT_TOP | DT_NOCLIP | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (g_ov->editCaretOn) {
        std::wstring prefix = g_ov->editText.substr(0, (size_t)g_ov->editCaret);
        SIZE sz{};
        if (!prefix.empty()) GetTextExtentPoint32W(dc, prefix.c_str(), (int)prefix.size(), &sz);
        int cx = x + sz.cx;
        HPEN cpen = CreatePen(PS_SOLID, 2, col);
        HGDIOBJ ocp = SelectObject(dc, cpen);
        MoveToEx(dc, cx, y + 1, NULL);
        LineTo(dc, cx, y + fsz + 2);
        SelectObject(dc, ocp);
        DeleteObject(cpen);
    }

    SelectObject(dc, of);
    DeleteObject(font);
}

LRESULT CALLBACK EditSub(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = (WNDPROC)GetPropW(hwnd, L"zpix_old_edit_proc");
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  // 彻底禁止绘制白底
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;  // 由覆盖层双缓冲直接渲染透明文字
    }
    case WM_SETFOCUS: {
        LRESULT r = old ? CallWindowProcW(old, hwnd, msg, wp, lp) : 0;
        HideCaret(hwnd);
        return r;
    }
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            CommitEdit(true);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            CommitEdit(false);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN) {
            CommitEdit(true);
            return 0;
        }
        break;
    case WM_KILLFOCUS:
        if (g_ov && !g_ov->keepEditOpen) CommitEdit(true);
        break;
    }

    LRESULT res = old ? CallWindowProcW(old, hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);

    if (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_IME_COMPOSITION ||
        msg == WM_IME_ENDCOMPOSITION || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR ||
        msg == WM_UNDO || msg == WM_LBUTTONUP) {
        if (IsWindow(hwnd)) HideCaret(hwnd);
        SyncEditFromControl();
    }
    return res;
}

void StartEdit(POINT ptScreen) {
    if (!g_ov) return;
    if (g_ov->hEdit) CommitEdit(true);

    g_ov->editPosScreen = ptScreen;
    g_ov->editText.clear();
    g_ov->editCaret = 0;
    g_ov->editCaretOn = true;
    g_ov->editVisualRect = ComputeEditVisualRect();

    int fsz = kTextSizes[g_ov->textSizeIdx];
    POINT client{ptScreen.x - g_ov->vs.left, ptScreen.y - g_ov->vs.top};
    g_ov->hEdit = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, client.x,
                                  client.y + fsz, 1, 1, g_ov->hwnd, (HMENU)700,
                                  GetModuleHandleW(NULL), NULL);
    // 把原生 EDIT 的可视区域裁为 0 像素：彻底杜绝其内部 GetDC 画白底，同时保留完整输入法/粘贴能力
    HRGN emptyRgn = CreateRectRgn(0, 0, 0, 0);
    SetWindowRgn(g_ov->hEdit, emptyRgn, FALSE);
    SendMessageW(g_ov->hEdit, EM_SETLIMITTEXT, 1024, 0);
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(g_ov->hEdit, GWLP_WNDPROC, (LONG_PTR)EditSub);
    SetPropW(g_ov->hEdit, L"zpix_old_edit_proc", (HANDLE)old);
    SetForegroundWindow(g_ov->hwnd);
    SetFocus(g_ov->hEdit);
    HideCaret(g_ov->hEdit);
    SetTimer(g_ov->hwnd, kTimerCaret, 530, NULL);

    RECT dirty = g_ov->editVisualRect;
    InflateRect(&dirty, 8, 8);
    InvalidateClientRect(g_ov->hwnd, dirty);
    ZLog(L"[ov] transparent text edit at %ld,%ld", client.x, client.y);
}

void CommitEdit(bool keep) {
    if (!g_ov || !g_ov->hEdit) return;
    HWND edit = g_ov->hEdit;
    RECT oldVis = g_ov->editVisualRect;
    g_ov->hEdit = nullptr;
    KillTimer(g_ov->hwnd, kTimerCaret);

    wchar_t buf[1024] = {};
    GetWindowTextW(edit, buf, 1024);
    DestroyWindow(edit);
    g_ov->editText.clear();
    g_ov->editVisualRect = RECT{};

    if (keep && buf[0]) {
        Ann a;
        a.type = AnnType::Text;
        a.a = g_ov->editPosScreen;
        a.b = a.a;
        a.text = buf;
        a.color = kPalette[g_ov->colorIdx];
        a.fontSize = kTextSizes[g_ov->textSizeIdx];
        g_ov->items.push_back(a);
        ZLog(L"[ov] text committed len=%d size=%d", (int)a.text.size(), a.fontSize);
    }
    if (g_ov->hwnd) {
        SetFocus(g_ov->hwnd);
        InflateRect(&oldVis, 16, 16);
        RECT dirty{};
        UnionRect(&dirty, &oldVis, &g_ov->sel);
        InflateRect(&dirty, 16, 16);
        InvalidateClientRect(g_ov->hwnd, dirty);
    }
}

// ---------------------------------------------------------------- painting

void PaintOverlay(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC screenDc = BeginPaint(hwnd, &ps);
    if (!g_ov) {
        EndPaint(hwnd, &ps);
        return;
    }

    RECT clip = ps.rcPaint;
    if (clip.right <= clip.left || clip.bottom <= clip.top) {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC dc = g_ov->backDC ? g_ov->backDC : screenDc;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);

    BitBlt(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, g_ov->dimDC,
           clip.left, clip.top, SRCCOPY);

    bool showLockedSel = (g_ov->phase == 2) || (g_ov->phase == 1 && g_ov->dragMoved);
    bool showHoverWin = !showLockedSel && g_ov->hasHoverWin;

    if (showLockedSel || showHoverWin) {
        RECT sel = showLockedSel ? g_ov->sel : g_ov->hoverWin;
        RECT visSel{};
        if (IntersectRect(&visSel, &sel, &clip)) {
            BitBlt(dc, visSel.left, visSel.top, visSel.right - visSel.left,
                   visSel.bottom - visSel.top, g_ov->shotDC, visSel.left, visSel.top, SRCCOPY);
            if (showLockedSel && g_ov->imgTrActive && g_ov->imgTrHasCache && g_ov->imgTrDC) {
                RECT trVis{};
                if (IntersectRect(&trVis, &visSel, &g_ov->imgTrSel)) {
                    BitBlt(dc, trVis.left, trVis.top, trVis.right - trVis.left,
                           trVis.bottom - trVis.top, g_ov->imgTrDC,
                           trVis.left - g_ov->imgTrSel.left, trVis.top - g_ov->imgTrSel.top,
                           SRCCOPY);
                }
            }
        }

        if (showLockedSel) {
            DrawAnnotations(dc, g_ov->items, POINT{-g_ov->vs.left, -g_ov->vs.top});
            if (g_ov->drawing) {
                std::vector<Ann> one{g_ov->draft};
                DrawAnnotations(dc, one, POINT{-g_ov->vs.left, -g_ov->vs.top});
            }
            if (g_ov->hEdit) DrawLiveEdit(dc);
        }

        if (showHoverWin) {
            HPEN pen = CreatePen(PS_SOLID, 3, kAccent);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, sel.left + 1, sel.top + 1, sel.right - 1, sel.bottom - 1);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);
        } else {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, sel.left, sel.top, sel.right, sel.bottom);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(pen);

            const int tick = 14, tw = 3;
            HPEN accent = CreatePen(PS_SOLID, tw, kAccent);
            op = SelectObject(dc, accent);
            int L = sel.left, T = sel.top, R = sel.right, B = sel.bottom;
            MoveToEx(dc, L, T, NULL);
            LineTo(dc, L + tick, T);
            MoveToEx(dc, L, T, NULL);
            LineTo(dc, L, T + tick);
            MoveToEx(dc, R, T, NULL);
            LineTo(dc, R - tick, T);
            MoveToEx(dc, R, T, NULL);
            LineTo(dc, R, T + tick);
            MoveToEx(dc, L, B, NULL);
            LineTo(dc, L + tick, B);
            MoveToEx(dc, L, B, NULL);
            LineTo(dc, L, B - tick);
            MoveToEx(dc, R, B, NULL);
            LineTo(dc, R - tick, B);
            MoveToEx(dc, R, B, NULL);
            LineTo(dc, R, B - tick);
            SelectObject(dc, op);
            DeleteObject(accent);

            // 锁定选区且未选标注工具时，在四角与四边中点画 8 个微调手柄（Snipaste/PixPin 风格）
            if (g_ov->phase == 2 && g_ov->tool == TOOL_NONE) {
                int mx = (L + R) / 2, my = (T + B) / 2;
                POINT hs[8] = {{L, T}, {mx, T}, {R, T}, {R, my},
                               {R, B}, {mx, B}, {L, B}, {L, my}};
                HPEN hp = CreatePen(PS_SOLID, 1, kAccent);
                HBRUSH hb = CreateSolidBrush(RGB(255, 255, 255));
                HGDIOBJ oldP = SelectObject(dc, hp);
                HGDIOBJ oldB = SelectObject(dc, hb);
                for (const POINT& h : hs) {
                    Rectangle(dc, h.x - 3, h.y - 3, h.x + 4, h.y + 4);
                }
                SelectObject(dc, oldP);
                SelectObject(dc, oldB);
                DeleteObject(hp);
                DeleteObject(hb);
            }
        }

        int w = sel.right - sel.left, h = sel.bottom - sel.top;
        wchar_t label[112];
        if (showHoverWin) {
            if (g_ov->hoverStack.size() > 1) {
                swprintf(label, 112, L"%d × %d  ·  单击选中 · 滚轮切换层级 (%d/%d)", w, h,
                         g_ov->hoverLevel + 1, (int)g_ov->hoverStack.size());
            } else {
                swprintf(label, 112, L"%d × %d  ·  单击选中区域", w, h);
            }
        } else if (g_ov->imgTrBusy) {
            swprintf(label, 112, L"%d × %d  ·  正在原位翻译图内文字…", w, h);
        } else if (!g_ov->imgTrNote.empty()) {
            swprintf(label, 112, L"%d × %d  ·  %ls", w, h, g_ov->imgTrNote.c_str());
        } else if (g_ov->imgTrActive && g_ov->imgTrHasCache) {
            swprintf(label, 112, L"%d × %d  ·  已图译 (按 G 切回原图)", w, h);
        } else if (g_ov->imgTrHasCache) {
            swprintf(label, 112, L"%d × %d  ·  原图 (按 G 显示译文)", w, h);
        } else {
            swprintf(label, 112, L"%d × %d", w, h);
        }
        HGDIOBJ of = SelectObject(dc, g_ov->fontDim);
        SetBkMode(dc, TRANSPARENT);
        SIZE sz{};
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &sz);
        int bx = sel.left;
        int by = sel.top - 30;
        if (by < 4) by = sel.top + 6;
        if (bx + sz.cx + 16 > g_ov->dim.w - 4) bx = g_ov->dim.w - sz.cx - 20;
        if (bx < 4) bx = 4;
        RECT lr{bx, by, bx + sz.cx + 16, by + 24};
        ui::RoundRect(dc, lr, 6, RGB(22, 23, 26), CLR_INVALID);
        SetTextColor(dc, RGB(245, 245, 245));
        DrawTextW(dc, label, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);

        if (g_ov->phase == 2) DrawToolbar(dc);
    }

    if (!showLockedSel) {
        const wchar_t* hint =
            L"智能识别网页 / 聊天框 / 控件    滚轮切换层级大小    拖动自由框选    Esc 取消";
        HGDIOBJ of = SelectObject(dc, g_ov->fontHint);
        SetBkMode(dc, TRANSPARENT);
        SIZE sz{};
        GetTextExtentPoint32W(dc, hint, (int)wcslen(hint), &sz);
        RECT mrc = g_ov->hintMonRect;
        if (mrc.right <= mrc.left) GetClientRect(hwnd, &mrc);
        int bx = mrc.left + ((mrc.right - mrc.left) - (sz.cx + 40)) / 2;
        int by = mrc.top + 24;
        RECT lr{bx, by, bx + sz.cx + 40, by + 40};
        ui::RoundRect(dc, lr, 9, RGB(28, 29, 32), kToolbarBorder);
        SetTextColor(dc, RGB(235, 236, 238));
        DrawTextW(dc, hint, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
    }

    if (g_ov->phase == 2 && g_ov->tool == TOOL_PICKER) {
        DrawColorLoupe(dc);
    }

    RestoreDC(dc, saved);

    if (dc != screenDc) {
        BitBlt(screenDc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, dc,
               clip.left, clip.top, SRCCOPY);
    }

    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- actions

static void ClearImgTranslate(OverlayState* st) {
    if (!st) return;
    ++st->imgTrSeq;
    st->imgTrBusy = false;
    st->imgTrHasCache = false;
    st->imgTrActive = false;
    st->imgTrSel = RECT{};
    st->imgTrNote.clear();
    st->imgTrPatch = Bitmap32{};
    if (st->imgTrDC) {
        SelectObject(st->imgTrDC, st->imgTrOld);
        DeleteDC(st->imgTrDC);
        st->imgTrDC = nullptr;
        st->imgTrOld = nullptr;
    }
    if (st->imgTrBmp) {
        DeleteObject(st->imgTrBmp);
        st->imgTrBmp = nullptr;
    }
}

static Bitmap32 MakeOverlayMosaicPatch(OverlayState* st, POINT aScreen, POINT bScreen, int block) {
    if (!st || !st->imgTrActive || !st->imgTrHasCache || st->imgTrPatch.empty()) {
        return MakeMosaicPatch(st->shot, aScreen, bScreen, block);
    }
    RECT rScreen = NormalizePointRect(aScreen, bScreen);
    RECT rClient = rScreen;
    OffsetRect(&rClient, -st->vs.left, -st->vs.top);

    RECT inter{};
    if (!IntersectRect(&inter, &rClient, &st->imgTrSel)) {
        return MakeMosaicPatch(st->shot, aScreen, bScreen, block);
    }

    int w = rClient.right - rClient.left;
    int h = rClient.bottom - rClient.top;
    if (w <= 0 || h <= 0) return Bitmap32{};

    Bitmap32 out;
    out.w = w;
    out.h = h;
    out.px.resize((size_t)w * h, 0xFF000000u);
    for (int y = 0; y < h; ++y) {
        int cy = rClient.top + y;
        if (cy < 0 || cy >= st->shot.h) continue;
        uint32_t* dstRow = out.row(y);
        for (int x = 0; x < w; ++x) {
            int cx = rClient.left + x;
            if (cx < 0 || cx >= st->shot.w) continue;
            if (cx >= st->imgTrSel.left && cx < st->imgTrSel.right && cy >= st->imgTrSel.top &&
                cy < st->imgTrSel.bottom) {
                int lx = cx - st->imgTrSel.left;
                int ly = cy - st->imgTrSel.top;
                if (lx >= 0 && lx < st->imgTrPatch.w && ly >= 0 && ly < st->imgTrPatch.h) {
                    dstRow[x] = st->imgTrPatch.row(ly)[lx];
                    continue;
                }
            }
            dstRow[x] = st->shot.row(cy)[cx];
        }
    }
    RECT local{0, 0, w, h};
    ApplyMosaic(out, local, block);
    return out;
}

static void RefreshImgTranslateVisuals(OverlayState* st) {
    if (!st || !st->hwnd) return;
    RECT oldBar = ToolbarRect();
    RECT oldTip = st->tipRect;
    LayoutToolbar();
    RECT newBar = ToolbarRect();
    RECT dirty = DecorExpandedRect(st->sel);
    if (oldBar.right > oldBar.left) UnionRect(&dirty, &dirty, &oldBar);
    if (newBar.right > newBar.left) UnionRect(&dirty, &dirty, &newBar);
    if (oldTip.right > oldTip.left) UnionRect(&dirty, &dirty, &oldTip);
    if (st->tipRect.right > st->tipRect.left) UnionRect(&dirty, &dirty, &st->tipRect);
    if (st->subBarRect.right > st->subBarRect.left) UnionRect(&dirty, &dirty, &st->subBarRect);
    InflateRect(&dirty, 6, 6);
    InvalidateClientRect(st->hwnd, dirty);
}

static bool IsSelCoveredByImgTr(const OverlayState* st) {
    if (!st || !st->imgTrHasCache || st->imgTrPatch.empty()) return false;
    return st->sel.left >= st->imgTrSel.left - 4 && st->sel.top >= st->imgTrSel.top - 4 &&
           st->sel.right <= st->imgTrSel.right + 4 && st->sel.bottom <= st->imgTrSel.bottom + 4;
}

static Bitmap32 BuildCurrentSelCrop(OverlayState* st) {
    RECT selScreen = SelScreenRect();
    Bitmap32 base = CropBitmap(st->shot, selScreen);
    if (base.empty()) return base;
    if (st->imgTrActive && st->imgTrHasCache && !st->imgTrPatch.empty()) {
        RECT ov{};
        if (IntersectRect(&ov, &st->sel, &st->imgTrSel)) {
            for (int cy = ov.top; cy < ov.bottom; ++cy) {
                int dstY = cy - st->sel.top;
                int srcY = cy - st->imgTrSel.top;
                if (dstY < 0 || dstY >= base.h || srcY < 0 || srcY >= st->imgTrPatch.h) continue;
                int dstX0 = ov.left - st->sel.left;
                int srcX0 = ov.left - st->imgTrSel.left;
                int copyW = ov.right - ov.left;
                if (dstX0 < 0) { copyW += dstX0; srcX0 -= dstX0; dstX0 = 0; }
                if (srcX0 < 0) { copyW += srcX0; dstX0 -= srcX0; srcX0 = 0; }
                if (dstX0 + copyW > base.w) copyW = base.w - dstX0;
                if (srcX0 + copyW > st->imgTrPatch.w) copyW = st->imgTrPatch.w - srcX0;
                if (copyW > 0) {
                    memcpy(base.row(dstY) + dstX0, st->imgTrPatch.row(srcY) + srcX0,
                           (size_t)copyW * 4);
                }
            }
        }
    }
    return base;
}

static void TriggerImgTranslate() {
    if (!g_ov || g_ov->phase != 2) return;
    if (g_ov->hEdit) CommitEdit(true);
    if (g_ov->imgTrBusy) return;

    int w = g_ov->sel.right - g_ov->sel.left;
    int h = g_ov->sel.bottom - g_ov->sel.top;
    if (w < 8 || h < 8) return;

    // 当前选区仍在已翻译覆盖范围内（含微调/收紧选区后）：一键零延迟切换「译文 ↔ 原图」
    if (IsSelCoveredByImgTr(g_ov)) {
        g_ov->imgTrActive = !g_ov->imgTrActive;
        g_ov->imgTrNote.clear();
        RefreshImgTranslateVisuals(g_ov);
        ZLog(L"[ov] img-tr toggle active=%d", (int)g_ov->imgTrActive);
        return;
    }

    ClearImgTranslate(g_ov);
    RECT selScreen = SelScreenRect();
    Bitmap32 crop = CropBitmap(g_ov->shot, selScreen);
    if (crop.empty()) return;

    g_ov->imgTrBusy = true;
    g_ov->imgTrSel = g_ov->sel;
    g_ov->imgTrNote.clear();
    uint64_t seq = ++g_ov->imgTrSeq;
    std::shared_ptr<ImgTrShared> shared = g_ov->imgTrShared;
    RefreshImgTranslateVisuals(g_ov);

    std::thread([shared, seq, crop]() {
        img_translate::Result res;
        bool ok = img_translate::TranslateSelectionImage(crop, res);
        if (!shared || !shared->alive.load() || !shared->notifyHwnd) return;
        ImgTrAsyncMsg* msg = new ImgTrAsyncMsg();
        msg->seq = seq;
        msg->ok = ok;
        msg->res = std::move(res);
        if (!PostMessageW(shared->notifyHwnd, WM_ZPIX_IMGTR_DONE, 0, (LPARAM)msg)) {
            delete msg;
        }
    }).detach();
}

void FinishWith(ShotAction action) {
    if (!g_ov) return;
    if (g_ov->hEdit) CommitEdit(true);
    int w = g_ov->sel.right - g_ov->sel.left;
    int h = g_ov->sel.bottom - g_ov->sel.top;
    if (w < 2 || h < 2) return;

    RECT selScreen = SelScreenRect();
    SetResultAnchor(selScreen);
    bool useTranslated =
        (action == ShotAction::CopyImage || action == ShotAction::Pin || action == ShotAction::Save) &&
        g_ov->imgTrActive && g_ov->imgTrHasCache && !g_ov->imgTrPatch.empty();

    Bitmap32 img;
    if (action == ShotAction::ImageSearch) {
        img = CropBitmap(g_ov->shot, selScreen);
    } else if (useTranslated) {
        img = RenderSelectionFromCrop(BuildCurrentSelCrop(g_ov), selScreen, g_ov->items);
    } else {
        img = RenderSelection(g_ov->shot, selScreen, g_ov->items);
    }
    ZLog(L"[ov] finish action=%d sel=%ld,%ld %dx%d -> %dx%d items=%d imgTr=%d", (int)action,
         g_ov->sel.left, g_ov->sel.top, w, h, img.w, img.h, (int)g_ov->items.size(),
         (int)useTranslated);

    DestroyWindow(g_ov->hwnd);
    DoShotAction(action, img, NULL);
}

void PickColorAndExit(POINT ptClient) {
    if (!g_ov) return;
    uint32_t px = SampleShotPixel(g_ov->shot, ptClient.x, ptClient.y);
    std::wstring val = FormatPickedColor(px, g_ov->pickerFmt);
    HWND hwnd = g_ov->hwnd;
    DestroyWindow(hwnd);
    CopyTextToClipboard(NULL, val);
    ShowToast(L"已复制颜色：" + val);
    ZLog(L"[ov] picked color=%ls fmt=%d", val.c_str(), g_cfg.pickerFormat);
}

void SetActiveTool(int newTool) {
    if (!g_ov) return;
    if (g_ov->hEdit && newTool != TOOL_TEXT) CommitEdit(true);

    RECT oldSub = g_ov->subBarRect;
    RECT oldLoupe = g_ov->loupeRect;
    RECT oldSel = DecorExpandedRect(g_ov->sel);

    g_ov->tool = (g_ov->tool == newTool) ? TOOL_NONE : newTool;
    g_ov->imgSearchSubOpen = false;
    g_ov->hoverSub = 0;
    g_ov->pressedSub = 0;
    LayoutSubToolbar();

    if (g_ov->tool == TOOL_PICKER) {
        POINT cur{};
        GetCursorPos(&cur);
        ScreenToClient(g_ov->hwnd, &cur);
        g_ov->mousePt = cur;
        g_ov->hasMousePt = true;
        g_ov->loupeRect = ComputeLoupeRect(cur);
    } else {
        g_ov->loupeRect = RECT{};
    }

    // 局部刷新：只刷新主工具栏、旧/新二级条、旧/新放大镜以及选区边框手柄，绝不整屏闪烁
    InvalidateToolbarArea(g_ov->hwnd);
    if (oldSub.right > oldSub.left) {
        InflateRect(&oldSub, 4, 4);
        InvalidateClientRect(g_ov->hwnd, oldSub);
    }
    if (oldLoupe.right > oldLoupe.left) {
        InflateRect(&oldLoupe, 4, 4);
        InvalidateClientRect(g_ov->hwnd, oldLoupe);
    }
    if (g_ov->loupeRect.right > g_ov->loupeRect.left) {
        RECT nl = g_ov->loupeRect;
        InflateRect(&nl, 4, 4);
        InvalidateClientRect(g_ov->hwnd, nl);
    }
    InvalidateClientRect(g_ov->hwnd, oldSel);
}

void ExecuteSubButton(int id) {
    if (!g_ov) return;
    for (const auto& sb : g_ov->subBtns) {
        if (sb.id != id) continue;
        if (sb.kind == SUB_COLOR) {
            g_ov->colorIdx = sb.value;
            InvalidateToolbarArea(g_ov->hwnd);
            if (g_ov->hEdit) {
                RECT er = g_ov->editVisualRect;
                InflateRect(&er, 8, 8);
                InvalidateClientRect(g_ov->hwnd, er);
            }
        } else if (sb.kind == SUB_SIZE) {
            if (g_ov->tool == TOOL_TEXT) {
                g_ov->textSizeIdx = sb.value;
                if (g_ov->hEdit) {
                    RECT oldEr = g_ov->editVisualRect;
                    g_ov->editVisualRect = ComputeEditVisualRect();
                    RECT u{};
                    UnionRect(&u, &oldEr, &g_ov->editVisualRect);
                    InflateRect(&u, 8, 8);
                    InvalidateClientRect(g_ov->hwnd, u);
                }
            } else if (g_ov->tool == TOOL_MOSAIC) {
                g_ov->mosaicIdx = sb.value;
            } else {
                g_ov->strokeIdx = sb.value;
            }
            InvalidateToolbarArea(g_ov->hwnd);
        } else if (sb.kind == SUB_PICKFMT) {
            g_ov->pickerFmt = sb.value;
            g_cfg.pickerFormat = sb.value;
            SaveSettings();
            InvalidateToolbarArea(g_ov->hwnd);
            if (g_ov->loupeRect.right > g_ov->loupeRect.left) {
                RECT lr = g_ov->loupeRect;
                InflateRect(&lr, 4, 4);
                InvalidateClientRect(g_ov->hwnd, lr);
            }
        } else if (sb.kind == SUB_IMGENGINE) {
            g_cfg.imageSearchEngine = sb.value;
            SaveSettings();
            FinishWith(ShotAction::ImageSearch);
        }
        return;
    }
}

void ExecuteButton(int id) {
    if (!g_ov) return;
    switch (id) {
    case BTN_COPY: FinishWith(ShotAction::CopyImage); break;
    case BTN_PIN: FinishWith(ShotAction::Pin); break;
    case BTN_SAVE: FinishWith(ShotAction::Save); break;
    case BTN_OCR: FinishWith(ShotAction::CopyText); break;
    case BTN_TRANSLATE: FinishWith(ShotAction::Translate); break;
    case BTN_IMG_TRANSLATE: TriggerImgTranslate(); break;
    case BTN_IMG_SEARCH: FinishWith(ShotAction::ImageSearch); break;
    case BTN_ARROW: SetActiveTool(TOOL_ARROW); break;
    case BTN_RECT: SetActiveTool(TOOL_RECT); break;
    case BTN_TEXT: SetActiveTool(TOOL_TEXT); break;
    case BTN_MOSAIC: SetActiveTool(TOOL_MOSAIC); break;
    case BTN_PICKER: SetActiveTool(TOOL_PICKER); break;
    case BTN_CLOSE: DestroyWindow(g_ov->hwnd); break;
    }
}

// ---------------------------------------------------------------- wndproc

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OverlayState* st = (OverlayState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        st = (OverlayState*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        return 0;
    }

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_TIMER:
        if (st && wp == kTimerCaret && st->hEdit) {
            st->editCaretOn = !st->editCaretOn;
            RECT er = st->editVisualRect;
            InflateRect(&er, 4, 4);
            InvalidateClientRect(hwnd, er);
            return 0;
        }
        break;

    case WM_SETCURSOR: {
        if (!st || LOWORD(lp) != HTCLIENT) break;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (st->phase == 2) {
            if (HitTestButton(pt) != 0 || HitTestSubButton(pt) != 0) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return TRUE;
            }
            if (PointInAnyToolbar(pt)) {
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
                return TRUE;
            }
            if (st->tool == TOOL_PICKER) {
                SetCursor(LoadCursorW(NULL, IDC_CROSS));
                return TRUE;
            }
            if (st->tool == TOOL_TEXT && PtInRect(&st->sel, pt)) {
                SetCursor(LoadCursorW(NULL, IDC_IBEAM));
                return TRUE;
            }
            if (st->tool == TOOL_NONE) {
                int h = (st->selDragMode != HIT_NONE) ? st->selDragMode
                                                      : HitTestSelection(st->sel, pt);
                SetCursor(LoadCursorW(NULL, CursorForSelHit(h)));
                return TRUE;
            }
        }
        SetCursor(LoadCursorW(NULL, IDC_CROSS));
        return TRUE;
    }

    case WM_PAINT:
        PaintOverlay(hwnd);
        return 0;

    case WM_MOUSEMOVE: {
        if (!st) break;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        st->mousePt = pt;
        st->hasMousePt = true;

        // 1) 选区外拖出新选区（防止单击误删原选区）
        if (st->reselectPending) {
            if (std::abs(pt.x - st->p0.x) >= 5 || std::abs(pt.y - st->p0.y) >= 5) {
                st->reselectPending = false;
                st->phase = 1;
                st->dragMoved = true;
                st->items.clear();
                ClearImgTranslate(st);
                st->tool = TOOL_NONE;
                st->imgSearchSubOpen = false;
                st->hover = 0;
                st->tipRect = RECT{};
                st->btns.clear();
                st->subBtns.clear();
                st->subBarRect = RECT{};
                st->p1 = pt;
                st->sel = NormalizePointRect(st->p0, st->p1);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        // 2) 选区平移 / 8 方向微调缩放
        if (st->selDragMode != HIT_NONE) {
            int dx = pt.x - st->selDragStart.x;
            int dy = pt.y - st->selDragStart.y;
            if (st->selDragMode == HIT_MOVE && !st->selDragMoved) {
                if (std::abs(dx) < 5 && std::abs(dy) < 5) return 0;
                st->selDragMoved = true;
            }
            RECT oldSel = st->sel;
            RECT oldBar = ToolbarRect();
            RECT oldSub = st->subBarRect;
            RECT oldTip = st->tipRect;
            st->hover = 0;
            RECT ns = st->selOrig;
            int maxW = st->dim.w;
            int maxH = st->dim.h;

            if (st->selDragMode == HIT_MOVE) {
                int w = ns.right - ns.left;
                int h = ns.bottom - ns.top;
                int nx = ns.left + dx;
                int ny = ns.top + dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx + w > maxW) nx = maxW - w;
                if (ny + h > maxH) ny = maxH - h;
                ns = RECT{nx, ny, nx + w, ny + h};
            } else {
                if (st->selDragMode == HIT_TL || st->selDragMode == HIT_L ||
                    st->selDragMode == HIT_BL)
                    ns.left += dx;
                if (st->selDragMode == HIT_TR || st->selDragMode == HIT_R ||
                    st->selDragMode == HIT_BR)
                    ns.right += dx;
                if (st->selDragMode == HIT_TL || st->selDragMode == HIT_T ||
                    st->selDragMode == HIT_TR)
                    ns.top += dy;
                if (st->selDragMode == HIT_BL || st->selDragMode == HIT_B ||
                    st->selDragMode == HIT_BR)
                    ns.bottom += dy;
                ns = NormalizePointRect(POINT{ns.left, ns.top}, POINT{ns.right, ns.bottom});
                if (ns.left < 0) ns.left = 0;
                if (ns.top < 0) ns.top = 0;
                if (ns.right > maxW) ns.right = maxW;
                if (ns.bottom > maxH) ns.bottom = maxH;
                if (ns.right - ns.left < 8) ns.right = std::min<LONG>(maxW, ns.left + 8);
                if (ns.bottom - ns.top < 8) ns.bottom = std::min<LONG>(maxH, ns.top + 8);
            }

            st->sel = ns;
            LayoutToolbar();

            RECT dirty{}, d1 = DecorExpandedRect(oldSel), d2 = DecorExpandedRect(st->sel);
            UnionRect(&dirty, &d1, &d2);
            RECT nb = ToolbarRect();
            if (oldBar.right > oldBar.left) UnionRect(&dirty, &dirty, &oldBar);
            if (nb.right > nb.left) UnionRect(&dirty, &dirty, &nb);
            if (oldSub.right > oldSub.left) UnionRect(&dirty, &dirty, &oldSub);
            if (oldTip.right > oldTip.left) UnionRect(&dirty, &dirty, &oldTip);
            if (st->subBarRect.right > st->subBarRect.left)
                UnionRect(&dirty, &dirty, &st->subBarRect);
            InflateRect(&dirty, 4, 4);
            InvalidateClientRect(hwnd, dirty);
            return 0;
        }

        // 3) 悬停识窗
        if (st->phase == 0) {
            bool hadOld = st->hasHoverWin;
            RECT oldWin = st->hoverWin;
            if (UpdateHoverWindow(st, pt)) {
                InvalidateTwoDecorRects(hwnd, hadOld, oldWin, st->hasHoverWin, st->hoverWin);
            }
            return 0;
        }

        // 4) 初始拖拽框选
        if (st->phase == 1) {
            st->p1 = pt;
            if (!st->dragMoved && (std::abs(pt.x - st->p0.x) >= 5 || std::abs(pt.y - st->p0.y) >= 5)) {
                st->dragMoved = true;
                st->sel = NormalizePointRect(st->p0, st->p1);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (st->dragMoved) {
                RECT oldSel = st->sel;
                st->sel = NormalizePointRect(st->p0, st->p1);
                InvalidateTwoDecorRects(hwnd, true, oldSel, true, st->sel);
            }
            return 0;
        }

        // 5) 标注绘制中
        if (st->drawing) {
            st->draft.b = POINT{pt.x + st->vs.left, pt.y + st->vs.top};
            RECT selDirty = st->sel;
            InflateRect(&selDirty, 24, 24);
            InvalidateClientRect(hwnd, selDirty);
            return 0;
        }

        // 6) 已锁定选区：工具栏 / 二级条悬停 + 识图厂家展开条 + 取色放大镜跟踪
        if (st->phase == 2) {
            int oldB = st->hover;
            int oldS = st->hoverSub;
            st->hover = HitTestButton(pt);
            st->hoverSub = HitTestSubButton(pt);

            bool subChanged = false;
            RECT oldSub = st->subBarRect;
            RECT oldTipBefore = st->tipRect;
            if (st->tool == TOOL_NONE) {
                bool wantSearchSub = (st->hover == BTN_IMG_SEARCH);
                // 仅当未悬停其他主按钮（hover == 0）且位于识图按钮正下/正上过渡带或二级条内部时保持展开；
                // 一旦移到其他主工具栏按钮，立即收起识图子平台栏
                if (!wantSearchSub && st->hover == 0 && st->imgSearchSubOpen &&
                    st->subBarRect.right > st->subBarRect.left) {
                    RECT bar = ToolbarRect();
                    RECT subPad = st->subBarRect;
                    InflateRect(&subPad, 4, 2);
                    if (PtInRect(&subPad, pt)) {
                        wantSearchSub = true;
                    } else {
                        RECT gapZone{};
                        if (st->subBarRect.top >= bar.bottom) {
                            gapZone = RECT{st->subBarRect.left - 4, bar.bottom,
                                           st->subBarRect.right + 4, st->subBarRect.top + 3};
                        } else if (st->subBarRect.bottom <= bar.top) {
                            gapZone = RECT{st->subBarRect.left - 4, st->subBarRect.bottom - 3,
                                           st->subBarRect.right + 4, bar.top};
                        }
                        if (gapZone.right > gapZone.left && PtInRect(&gapZone, pt)) {
                            wantSearchSub = true;
                        } else {
                            for (const auto& b : st->btns) {
                                if (b.id == BTN_IMG_SEARCH) {
                                    RECT btnRim{b.rc.left - 2, bar.top, b.rc.right + 2, bar.bottom};
                                    if (PtInRect(&btnRim, pt)) wantSearchSub = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (wantSearchSub != st->imgSearchSubOpen) {
                    st->imgSearchSubOpen = wantSearchSub;
                    st->hoverSub = 0;
                    LayoutSubToolbar();
                    st->hoverSub = HitTestSubButton(pt);
                    subChanged = true;
                }
            } else if (st->imgSearchSubOpen) {
                st->imgSearchSubOpen = false;
                subChanged = true;
            }

            if (oldB != st->hover || oldS != st->hoverSub || subChanged) {
                InvalidateToolbarArea(hwnd);
                if (subChanged) {
                    if (oldSub.right > oldSub.left) {
                        InflateRect(&oldSub, 4, 4);
                        InvalidateClientRect(hwnd, oldSub);
                    }
                    if (oldTipBefore.right > oldTipBefore.left) {
                        InflateRect(&oldTipBefore, 4, 4);
                        InvalidateClientRect(hwnd, oldTipBefore);
                    }
                }
            }

            if (st->tool == TOOL_PICKER) {
                RECT oldL = st->loupeRect;
                st->loupeRect = PointInAnyToolbar(pt) ? RECT{} : ComputeLoupeRect(pt);
                RECT dirty{};
                bool hasOld = oldL.right > oldL.left;
                bool hasNew = st->loupeRect.right > st->loupeRect.left;
                if (hasOld && hasNew) UnionRect(&dirty, &oldL, &st->loupeRect);
                else if (hasOld) dirty = oldL;
                else if (hasNew) dirty = st->loupeRect;
                if (hasOld || hasNew) {
                    InflateRect(&dirty, 4, 4);
                    InvalidateClientRect(hwnd, dirty);
                }
            }
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!st) break;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        if (st->phase == 2) {
            // 先判二级属性条（文字编辑中点颜色/字号时不关闭输入框！）
            int subId = HitTestSubButton(pt);
            if (subId) {
                st->keepEditOpen = (st->hEdit != nullptr);
                st->pressedSub = subId;
                SetCapture(hwnd);
                InvalidateToolbarArea(hwnd);
                return 0;
            }
            if (st->subBarRect.right > st->subBarRect.left && PtInRect(&st->subBarRect, pt)) {
                return 0;
            }

            // 再判主工具栏
            int id = HitTestButton(pt);
            if (id) {
                st->pressed = id;
                SetCapture(hwnd);
                InvalidateToolbarArea(hwnd);
                return 0;
            }
            RECT bar = ToolbarRect();
            if (PtInRect(&bar, pt)) return 0;

            // 取色器模式：点击任意非工具栏位置直接复制色值并退出截图！
            if (st->tool == TOOL_PICKER) {
                PickColorAndExit(pt);
                return 0;
            }

            // 无标注工具时：选区内拖动平移、边框 8 手柄缩放，单击/长按不再消失！
            if (st->tool == TOOL_NONE) {
                int hit = HitTestSelection(st->sel, pt);
                if (hit != HIT_NONE) {
                    st->selDragMode = hit;
                    st->selDragMoved = false;
                    st->selDragStart = pt;
                    st->selOrig = st->sel;
                    SetCapture(hwnd);
                    return 0;
                }
                // 点在选区外：只有拖动 >= 5px 才开启新框选，误触单击不销毁原选区
                st->reselectPending = true;
                st->p0 = st->p1 = pt;
                SetCapture(hwnd);
                return 0;
            }

            // 标注工具模式
            if (PtInRect(&st->sel, pt)) {
                POINT sp{pt.x + st->vs.left, pt.y + st->vs.top};
                if (st->tool == TOOL_TEXT) {
                    StartEdit(sp);
                    return 0;
                }
                SetCapture(hwnd);
                st->drawing = true;
                st->draft = Ann{};
                st->draft.type = (st->tool == TOOL_ARROW)
                                     ? AnnType::Arrow
                                     : (st->tool == TOOL_RECT ? AnnType::Rect : AnnType::Mosaic);
                st->draft.a = sp;
                st->draft.b = sp;
                st->draft.color = kPalette[st->colorIdx];
                st->draft.width = kStrokeWidths[st->strokeIdx];
                st->draft.block = kMosaicBlocks[st->mosaicIdx];
                RECT selDirty = st->sel;
                InflateRect(&selDirty, 24, 24);
                InvalidateClientRect(hwnd, selDirty);
                return 0;
            }

            // 标注工具激活时点选区外：若在打字则提交文字，但不销毁选区
            if (st->hEdit) CommitEdit(true);
            return 0;
        }

        // phase == 0：准备单击锁窗或拖动新建选区
        SetCapture(hwnd);
        st->phase = 1;
        st->dragMoved = false;
        st->p0 = st->p1 = pt;
        st->sel = NormalizePointRect(pt, pt);
        UpdateHoverWindow(st, pt);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!st) break;
        ReleaseCapture();
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        if (st->pressedSub) {
            int subId = st->pressedSub;
            st->pressedSub = 0;
            if (HitTestSubButton(pt) == subId) ExecuteSubButton(subId);
            if (st->hEdit) {
                SetFocus(st->hEdit);
                HideCaret(st->hEdit);
            }
            st->keepEditOpen = false;
            InvalidateToolbarArea(hwnd);
            return 0;
        }
        if (st->pressed) {
            int id = st->pressed;
            st->pressed = 0;
            if (HitTestButton(pt) == id) {
                ExecuteButton(id);
                return 0;
            }
            InvalidateToolbarArea(hwnd);
            return 0;
        }
        if (st->reselectPending) {
            st->reselectPending = false;
            return 0;
        }
        if (st->selDragMode != HIT_NONE) {
            st->selDragMode = HIT_NONE;
            return 0;
        }
        if (st->drawing) {
            st->drawing = false;
            RECT r = NormalizePointRect(st->draft.a, st->draft.b);
            if (r.right - r.left > 3 || r.bottom - r.top > 3) {
                if (st->draft.type == AnnType::Mosaic) {
                    if (r.right - r.left > 3 && r.bottom - r.top > 3) {
                        st->draft.patch = MakeOverlayMosaicPatch(st, st->draft.a, st->draft.b,
                                                                 st->draft.block);
                        st->draft.cached = !st->draft.patch.empty();
                        st->items.push_back(st->draft);
                    }
                } else {
                    st->items.push_back(st->draft);
                }
                ZLog(L"[ov] ann type=%d items=%d", (int)st->draft.type, (int)st->items.size());
            }
            RECT selDirty = st->sel;
            InflateRect(&selDirty, 24, 24);
            InvalidateClientRect(hwnd, selDirty);
            return 0;
        }
        if (st->phase == 1) {
            if (!st->dragMoved && st->hasHoverWin &&
                st->hoverWin.right - st->hoverWin.left >= 4 &&
                st->hoverWin.bottom - st->hoverWin.top >= 4) {
                st->sel = st->hoverWin;
                st->phase = 2;
                LayoutToolbar();
                ZLog(L"[ov] window locked %ld,%ld %dx%d", st->sel.left, st->sel.top,
                     st->sel.right - st->sel.left, st->sel.bottom - st->sel.top);
            } else if (st->dragMoved && st->sel.right - st->sel.left >= 4 &&
                       st->sel.bottom - st->sel.top >= 4) {
                st->phase = 2;
                LayoutToolbar();
                ZLog(L"[ov] selected %ld,%ld %dx%d", st->sel.left, st->sel.top,
                     st->sel.right - st->sel.left, st->sel.bottom - st->sel.top);
            } else {
                st->phase = 0;
                UpdateHoverWindow(st, pt);
            }
            st->dragMoved = false;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }

    case WM_ZPIX_UIA_READY: {
        UiaBatchResult* batch = (UiaBatchResult*)lp;
        if (batch) {
            if (st) {
                for (auto& tw : st->topWins) {
                    if (tw.hwnd == batch->topHwnd) {
                        for (const RECT& r : batch->rects) {
                            AppendUniqueRect(tw.subRects, r, 3);
                        }
                        break;
                    }
                }
                if (st->phase == 0 && st->hasMousePt) {
                    bool hadOld = st->hasHoverWin;
                    RECT oldWin = st->hoverWin;
                    if (UpdateHoverWindow(st, st->mousePt, false)) {
                        InvalidateTwoDecorRects(hwnd, hadOld, oldWin, st->hasHoverWin,
                                                st->hoverWin);
                    }
                }
            }
            delete batch;
        }
        return 0;
    }

    case WM_ZPIX_IMGTR_DONE: {
        ImgTrAsyncMsg* msg = (ImgTrAsyncMsg*)lp;
        if (msg) {
            if (st && st->phase == 2 && msg->seq == st->imgTrSeq) {
                st->imgTrBusy = false;
                if (msg->ok && !msg->res.image.empty()) {
                    if (st->imgTrDC) {
                        SelectObject(st->imgTrDC, st->imgTrOld);
                        DeleteDC(st->imgTrDC);
                        st->imgTrDC = nullptr;
                        st->imgTrOld = nullptr;
                    }
                    if (st->imgTrBmp) {
                        DeleteObject(st->imgTrBmp);
                        st->imgTrBmp = nullptr;
                    }
                    st->imgTrPatch = std::move(msg->res.image);
                    st->imgTrDC = CreateBitmapDC(st->imgTrPatch, st->imgTrBmp, st->imgTrOld);
                    st->imgTrHasCache = (st->imgTrDC != nullptr);
                    st->imgTrActive = st->imgTrHasCache;
                    st->imgTrNote.clear();
                } else {
                    st->imgTrHasCache = false;
                    st->imgTrActive = false;
                    st->imgTrNote =
                        msg->res.error.empty() ? L"图内翻译失败，请重试" : msg->res.error;
                }
                RefreshImgTranslateVisuals(st);
            }
            delete msg;
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (st && st->phase == 0 && st->hoverStack.size() > 1) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int nextLvl = st->hoverLevel + (delta > 0 ? 1 : -1);
            if (nextLvl < 0) nextLvl = 0;
            if (nextLvl >= (int)st->hoverStack.size()) nextLvl = (int)st->hoverStack.size() - 1;
            if (nextLvl != st->hoverLevel) {
                RECT oldWin = st->hoverWin;
                st->hoverLevel = nextLvl;
                st->hoverWin = st->hoverStack[st->hoverLevel];
                InvalidateTwoDecorRects(hwnd, true, oldWin, true, st->hoverWin);
            }
            return 0;
        }
        break;
    }

    case WM_RBUTTONDOWN: {
        if (!st) break;
        if (st->hEdit) {
            CommitEdit(false);
            return 0;
        }
        if (st->phase == 2 && st->tool != TOOL_NONE) {
            // 有标注工具或取色器激活时，右键先退出当前工具回到选区微调态
            SetActiveTool(TOOL_NONE);
            return 0;
        }
        if (st->phase == 2) {
            st->phase = 0;
            st->dragMoved = false;
            st->selDragMode = HIT_NONE;
            st->reselectPending = false;
            st->items.clear();
            ClearImgTranslate(st);
            st->tool = TOOL_NONE;
            st->imgSearchSubOpen = false;
            st->btns.clear();
            st->subBtns.clear();
            st->subBarRect = RECT{};
            st->loupeRect = RECT{};
            POINT cur{};
            GetCursorPos(&cur);
            ScreenToClient(hwnd, &cur);
            UpdateHoverWindow(st, cur);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_KEYDOWN: {
        if (!st) break;
        UINT mods = 0, vk = 0;
        bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
        bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        auto matchHotkey = [&](const std::wstring& spec) {
            if (!ParseHotkey(spec, &mods, &vk) || wp != vk) return false;
            return ctrlDown == ((mods & MOD_CONTROL) != 0) &&
                   altDown == ((mods & MOD_ALT) != 0) &&
                   shiftDown == ((mods & MOD_SHIFT) != 0);
        };

        if (wp == VK_ESCAPE) {
            if (st->hEdit) {
                CommitEdit(false);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (st->hEdit) {
                CommitEdit(true);
                return 0;
            }
            if (st->phase == 2) FinishWith(ShotAction::Pin);
            return 0;
        }
        if (st->phase != 2) return 0;

        // 取色器模式按 Tab 快速循环切换 HEX/HEXA/RGB/RGBA/HSL/HSLA
        if (wp == VK_TAB && st->tool == TOOL_PICKER) {
            st->pickerFmt = (st->pickerFmt + 1) % 6;
            g_cfg.pickerFormat = st->pickerFmt;
            SaveSettings();
            InvalidateToolbarArea(hwnd);
            if (st->loupeRect.right > st->loupeRect.left) {
                RECT lr = st->loupeRect;
                InflateRect(&lr, 4, 4);
                InvalidateClientRect(hwnd, lr);
            }
            return 0;
        }

        // 方向键 1px 微调选区（Shift+方向键微调宽高）
        if (!st->hEdit && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)) {
            int step = ctrlDown ? 5 : 1;
            int dx = (wp == VK_LEFT) ? -step : (wp == VK_RIGHT ? step : 0);
            int dy = (wp == VK_UP) ? -step : (wp == VK_DOWN ? step : 0);
            RECT oldSel = st->sel;
            RECT oldBar = ToolbarRect();
            RECT oldSub = st->subBarRect;
            RECT oldTip = st->tipRect;
            st->hover = 0;
            if (shiftDown) {
                st->sel.right = std::max((int)st->sel.left + 8,
                                         std::min(st->dim.w, (int)st->sel.right + dx));
                st->sel.bottom = std::max((int)st->sel.top + 8,
                                          std::min(st->dim.h, (int)st->sel.bottom + dy));
            } else {
                int w = st->sel.right - st->sel.left;
                int h = st->sel.bottom - st->sel.top;
                int nx = std::max(0, std::min(st->dim.w - w, (int)st->sel.left + dx));
                int ny = std::max(0, std::min(st->dim.h - h, (int)st->sel.top + dy));
                st->sel = RECT{nx, ny, nx + w, ny + h};
            }
            LayoutToolbar();
            RECT dirty{}, d1 = DecorExpandedRect(oldSel), d2 = DecorExpandedRect(st->sel);
            UnionRect(&dirty, &d1, &d2);
            RECT nb = ToolbarRect();
            if (oldBar.right > oldBar.left) UnionRect(&dirty, &dirty, &oldBar);
            if (nb.right > nb.left) UnionRect(&dirty, &dirty, &nb);
            if (oldSub.right > oldSub.left) UnionRect(&dirty, &dirty, &oldSub);
            if (oldTip.right > oldTip.left) UnionRect(&dirty, &dirty, &oldTip);
            if (st->subBarRect.right > st->subBarRect.left)
                UnionRect(&dirty, &dirty, &st->subBarRect);
            InflateRect(&dirty, 4, 4);
            InvalidateClientRect(hwnd, dirty);
            return 0;
        }

        if (wp == 'Z' && ctrlDown && !altDown && !shiftDown) {
            if (!st->items.empty()) {
                st->items.pop_back();
                RECT selDirty = st->sel;
                InflateRect(&selDirty, 24, 24);
                InvalidateClientRect(hwnd, selDirty);
            } else if (st->imgTrActive) {
                st->imgTrActive = false;
                RefreshImgTranslateVisuals(st);
            }
            return 0;
        }
        if (wp == 'S' && ctrlDown && !altDown && !shiftDown) {
            FinishWith(ShotAction::Save);
            return 0;
        }
        if (matchHotkey(g_cfg.copyImage)) {
            FinishWith(ShotAction::CopyImage);
            return 0;
        }
        if (matchHotkey(g_cfg.pin)) {
            FinishWith(ShotAction::Pin);
            return 0;
        }
        if (matchHotkey(g_cfg.copyText)) {
            FinishWith(ShotAction::CopyText);
            return 0;
        }
        if (matchHotkey(g_cfg.translate)) {
            FinishWith(ShotAction::Translate);
            return 0;
        }
        if (!ctrlDown && !altDown && !st->hEdit) {
            if (wp == 'S') {
                ExecuteButton(BTN_IMG_SEARCH);
                return 0;
            }
            if (wp == 'G') {
                ExecuteButton(BTN_IMG_TRANSLATE);
                return 0;
            }
            if (wp == 'C') {
                ExecuteButton(BTN_PICKER);
                return 0;
            }
            if (wp == 'A') {
                ExecuteButton(BTN_ARROW);
                return 0;
            }
            if (wp == 'R') {
                ExecuteButton(BTN_RECT);
                return 0;
            }
            if (wp == 'T') {
                ExecuteButton(BTN_TEXT);
                return 0;
            }
            if (wp == 'M') {
                ExecuteButton(BTN_MOSAIC);
                return 0;
            }
        }
        return 0;
    }

    case WM_DESTROY: {
        if (st) {
            if (st->uiaState) st->uiaState->alive = false;
            if (st->imgTrShared) st->imgTrShared->alive = false;
            ClearImgTranslate(st);
            KillTimer(hwnd, kTimerCaret);
            if (st->hEdit) st->hEdit = nullptr;
            if (st->shotDC) {
                SelectObject(st->shotDC, st->shotOld);
                DeleteDC(st->shotDC);
            }
            if (st->shotBmp) DeleteObject(st->shotBmp);
            if (st->dimDC) {
                SelectObject(st->dimDC, st->dimOld);
                DeleteDC(st->dimDC);
            }
            if (st->dimBmp) DeleteObject(st->dimBmp);
            if (st->backDC) {
                SelectObject(st->backDC, st->backOld);
                DeleteDC(st->backDC);
            }
            if (st->backBmp) DeleteObject(st->backBmp);
            if (st->fontBtn) DeleteObject(st->fontBtn);
            if (st->fontKey) DeleteObject(st->fontKey);
            if (st->fontDim) DeleteObject(st->fontDim);
            if (st->fontHint) DeleteObject(st->fontHint);
            if (st->fontSmall) DeleteObject(st->fontSmall);
            if (st == g_ov) g_ov = nullptr;
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool IsOverlayActive() { return g_ov != nullptr && IsWindow(g_ov->hwnd); }

void StartScreenshot() {
    if (IsOverlayActive()) return;
    HideSelectBar();

    OverlayState* st = new OverlayState();
    st->pickerFmt = (g_cfg.pickerFormat >= 0 && g_cfg.pickerFormat <= 5) ? g_cfg.pickerFormat : 0;

    if (!CaptureVirtualScreen(st->shot)) {
        ZLog(L"[ov] capture failed");
        delete st;
        return;
    }
    st->vs = GetVirtualScreenRect();
    SnapshotVisibleWindows(st);
    {
        POINT cur{};
        GetCursorPos(&cur);
        RECT mwork = MonitorWorkRect(cur);
        OffsetRect(&mwork, -st->vs.left, -st->vs.top);
        st->hintMonRect = mwork;
        POINT curClient{cur.x - st->vs.left, cur.y - st->vs.top};
        st->mousePt = curClient;
        st->hasMousePt = true;
        UpdateHoverWindow(st, curClient);
    }

    st->dim = st->shot;
    for (auto& p : st->dim.px) {
        uint32_t r = ((p >> 16) & 0xFF) * 45 / 100;
        uint32_t g = ((p >> 8) & 0xFF) * 45 / 100;
        uint32_t b = (p & 0xFF) * 45 / 100;
        p = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    st->shotDC = CreateBitmapDC(st->shot, st->shotBmp, st->shotOld);
    st->dimDC = CreateBitmapDC(st->dim, st->dimBmp, st->dimOld);
    {
        HDC screen = GetDC(NULL);
        st->backDC = CreateCompatibleDC(screen);
        st->backBmp = CreateCompatibleBitmap(screen, st->dim.w, st->dim.h);
        if (st->backDC && st->backBmp) {
            st->backOld = SelectObject(st->backDC, st->backBmp);
        } else {
            if (st->backBmp) {
                DeleteObject(st->backBmp);
                st->backBmp = nullptr;
            }
            if (st->backDC) {
                DeleteDC(st->backDC);
                st->backDC = nullptr;
            }
        }
        ReleaseDC(NULL, screen);
    }
    st->fontBtn = MakeUiFont(14);
    st->fontKey = MakeUiFont(9);
    st->fontDim = MakeUiFont(14, true);
    st->fontHint = MakeUiFont(15);
    st->fontSmall = MakeUiFont(12, true);

    static bool clsDone = false;
    if (!clsDone) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
        wc.lpszClassName = L"ZpixOverlay";
        RegisterClassExW(&wc);
        clsDone = true;
    }

    g_ov = st;
    st->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ZpixOverlay", L"Zpix", WS_POPUP,
                               st->vs.left, st->vs.top, st->vs.right - st->vs.left,
                               st->vs.bottom - st->vs.top, NULL, NULL, GetModuleHandleW(NULL), st);
    if (!st->hwnd) {
        ZLog(L"[ov] CreateWindow failed err=%lu", GetLastError());
        delete st;
        g_ov = nullptr;
        return;
    }

    st->uiaState = std::make_shared<UiaSharedState>();
    st->uiaState->notifyHwnd = st->hwnd;
    st->uiaState->vs = st->vs;
    st->imgTrShared = std::make_shared<ImgTrShared>();
    st->imgTrShared->notifyHwnd = st->hwnd;
    // 立即预热光标所在窗口与最前方可见窗口的 UIA 树（网页 div / 聊天控件等）
    if (st->hasMousePt) {
        for (auto& tw : st->topWins) {
            if (PtInRect(&tw.rcClient, st->mousePt)) {
                QueueUiaForWindow(st, tw);
                break;
            }
        }
    }
    for (size_t i = 0; i < st->topWins.size() && i < 3; ++i) {
        QueueUiaForWindow(st, st->topWins[i]);
    }

    ShowWindow(st->hwnd, SW_SHOW);
    UpdateWindow(st->hwnd);
    SetForegroundWindow(st->hwnd);
    SetFocus(st->hwnd);
    ZLog(L"[ov] overlay hwnd=%p rect=%ld,%ld %ldx%ld", (void*)st->hwnd, st->vs.left, st->vs.top,
         st->vs.right - st->vs.left, st->vs.bottom - st->vs.top);
}
