#include "actions.h"
#include "annotate.h"
#include "config.h"
#include "img_search.h"
#include "ocr_paddle.h"
#include "pin.h"
#include "settings.h"
#include "ui.h"
#include "translate.h"

#include <winhttp.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <shellapi.h>
#include <vector>

#pragma comment(lib, "winhttp.lib")

// ---------------------------------------------------------------- helpers

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    return path;
}

std::wstring ExeDirOf() { return ExeDir(); }

bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text) {
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), len, NULL, NULL);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    return true;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* p = GlobalLock(mem);
        memcpy(p, text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    return true;
}

static std::string WideToUtf8(const std::wstring& s) {
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len, NULL, NULL);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

// ---------------------------------------------------------------- OCR script

static std::string RunCapture(const std::wstring& cmdline, DWORD timeoutMs) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(NULL, cmd.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si,
                             &pi);
    if (!ok) {
        ZLog(L"[proc] CreateProcess failed err=%lu", GetLastError());
        return "";
    }
    DWORD st = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (st != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        ZLog(L"[proc] timeout");
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return "";
}
static std::wstring ReadTextFileUtf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string data((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    if (sz.QuadPart > 0) ReadFile(h, data.data(), (DWORD)sz.QuadPart, &read, NULL);
    CloseHandle(h);
    data.resize(read);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF)
        data.erase(0, 3);
    return Utf8ToWide(data);
}

std::wstring OcrImageFile(const std::wstring& pngPath) {
    static std::mutex ocrMu;
    std::lock_guard<std::mutex> lk(ocrMu);

    // 1) 本地 PP-OCRv4（onnxruntime）：中文小字与粗体拉丁字母都比系统引擎准，速度快一个数量级
    {
        static bool tried = false;
        static bool ready = false;
        if (!tried) {
            tried = true;
            std::wstring modelDir = ExeDir() + L"model\\";   // 模型跟 exe 走（安装版/绿色版都在这）
            ready = ocr_paddle::Init(modelDir);
            ZLog(L"[ocr] paddle init=%d dir=%ls", (int)ready, modelDir.c_str());
            if (!ready) {
                // 数据目录也认（用户自己放的模型）
                std::wstring alt = DataDir() + L"model\\";
                if (alt != modelDir) {
                    ready = ocr_paddle::Init(alt);
                    if (ready) ZLog(L"[ocr] paddle init via %ls", alt.c_str());
                }
            }
        }
        if (ready) {
            int ms = 0;
            double conf = 0;
            std::wstring t = ocr_paddle::RecognizeFile(pngPath, &ms, &conf);
            if (!t.empty()) {
                ZLog(L"[ocr] paddle chars=%d ms=%d conf=%.2f", (int)t.size(), ms, conf);
                return t;
            }
            ZLog(L"[ocr] paddle empty -> fallback powershell");
        }
    }

    std::wstring script = DataDir() + L"ocr.ps1";
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) script = ExeDir() + L"ocr.ps1";
    if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        ZLog(L"[ocr] script missing: %ls", script.c_str());
        return L"";
    }

    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring ps = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    static std::atomic<uint64_t> outSeq{0};
    std::wstring outFile =
        DataDir() + L"ocr_out_" + std::to_wstring(++outSeq) + L".txt";
    DeleteFileW(outFile.c_str());
    std::wstring errFile = outFile + L".err";
    DeleteFileW(errFile.c_str());

    std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\" -Path \"" +
                          pngPath + L"\" -Out \"" + outFile + L"\"";
    ZLog(L"[ocr] exec: %ls %ls", ps.c_str(), params.c_str());

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = ps.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 25000);
        CloseHandle(sei.hProcess);
    } else {
        ZLog(L"[ocr] ShellExecuteEx failed err=%lu", GetLastError());
    }

    std::wstring text = ReadTextFileUtf8(outFile);
    std::wstring err = ReadTextFileUtf8(errFile);
    DeleteFileW(outFile.c_str());
    DeleteFileW(errFile.c_str());
    if (text.empty() && !err.empty()) ZLog(L"[ocr] script error: %ls", err.substr(0, 300).c_str());

    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    ZLog(L"[ocr] result chars=%d", (int)text.size());
    return text;
}

// ---------------------------------------------------------------- result window
// 多厂家翻译结果窗口：无边框 + 全自绘（标题栏 / 原文卡片 / 每个厂家一张结果卡片）
// 视觉：纯白底、#F7F8FA 分区、#F09015 橙点缀、10px 圆角、细边框、细滚动条
// 说明：这里只做绘制与交互层，OCR / 翻译请求 / 结果数据结构全部沿用原逻辑

HWND g_resultWnd = nullptr;

static const COLORREF kCardBorder = RGB(237, 239, 242);
static const COLORREF kCardBorderHot = RGB(226, 229, 235);
static const COLORREF kThumb = RGB(223, 226, 231);
static const COLORREF kThumbHot = RGB(198, 203, 211);
static const COLORREF kErrText = RGB(192, 86, 75);

struct ResultState {
    HWND hwnd = nullptr;
    int scale = 100;
    int width = 720;
    int height = 520;
    RECT work{};
    std::wstring original;
    std::vector<TrResult> results;
    bool pending = true;
    bool selectMode = false;
    std::wstring overrideLang;  // 空=自动切换；非空=用户手动指定的语言代码
    std::wstring activeLang;    // 当前生效的目标语言代码
    uint64_t reqId = 1;

    // ---- 布局（内容坐标系：卡片用滚动前的坐标，绘制时加偏移）
    RECT rcClose{}, rcCopyAll{}, rcLangBtn{}, rcSrcCard{}, rcSrcCopy{}, rcList{}, rcBarThumb{};
    std::vector<RECT> rcCard, rcCopy, rcPanel;
    std::vector<int> cardTextH, cardViewH, cardScroll;
    int srcTextH = 0, srcViewH = 0, srcScroll = 0;
    int contentH = 0, scrollY = 0, maxScroll = 0;

    // ---- 交互状态
    int hover = -100;       // -100 无 / -3 关闭 / -2 复制全部 / -4 复制原文 / >=0 卡片复制
    int cardHover = -1;
    int pressed = -100;
    int copied = -100;
    DWORD copiedUntil = 0;
    DWORD barUntil = 0;
    bool barHot = false;
    bool tracking = false;
    bool dragBar = false;
    int dragOriginY = 0, dragOriginScroll = 0;
    bool winDragging = false;
    bool inSizing = false;
    double dragRx = 0.5;
    double dragRy = 0.05;
    int dragOffX = 0;
    int dragOffY = 0;

    HFONT fTitle = nullptr, fName = nullptr, fTag = nullptr, fTagB = nullptr, fMeta = nullptr,
          fBody = nullptr, fSrc = nullptr, fBtn = nullptr;
};

static ResultState* g_rs = nullptr;

static int RS(int v) { return g_rs ? MulDiv(v, g_rs->scale, 100) : v; }

// 本次操作的落点（选区中心）。多屏下用它挑显示器，避免窗口跑到别块屏 / 跨界缝上。
static POINT g_anchor{INT_MIN, INT_MIN};
void SetResultAnchor(RECT selScreen) {
    g_anchor.x = (selScreen.left + selScreen.right) / 2;
    g_anchor.y = (selScreen.top + selScreen.bottom) / 2;
    ZLog(L"[res] anchor=%ld,%ld", g_anchor.x, g_anchor.y);
}

// 取某点所在显示器的工作区 + 该显示器自身的 DPI（不是主屏 DPI）
// 关键：只用单块屏的 rcWork。以前拿 GetVirtualScreenRect() 兜底，两块屏横排时
// 虚拟桌面中心正好落在两屏中间的缝上，窗口就会"一边一半"。
static void MonitorOfWorkArea(POINT pt, RECT* work, int* scale) {
    *work = MonitorWorkRect(pt);
    *scale = MonitorScaleOf(pt);
    ZLog(L"[res] monitor work=%ld,%ld-%ld,%ld scale=%d", work->left, work->top, work->right,
         work->bottom, *scale);
}

static void ResultApplyScale() {
    if (!g_rs) return;
    ZLog(L"[res] apply scale=%d", g_rs->scale);
    HFONT* fonts[] = {&g_rs->fTitle, &g_rs->fName, &g_rs->fTag, &g_rs->fTagB,
                      &g_rs->fMeta,  &g_rs->fBody, &g_rs->fSrc, &g_rs->fBtn};
    for (HFONT* f : fonts)
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
    int s = g_rs->scale;
    g_rs->fTitle = MakeUiFont(MulDiv(14, s, 100), true);
    g_rs->fName = MakeUiFont(MulDiv(14, s, 100), true);
    g_rs->fTag = MakeUiFont(MulDiv(11, s, 100), false);
    g_rs->fTagB = MakeUiFont(MulDiv(11, s, 100), true);
    g_rs->fMeta = MakeUiFont(MulDiv(11, s, 100), false);
    g_rs->fBody = MakeUiFont(MulDiv(14, s, 100), false);
    g_rs->fSrc = MakeUiFont(MulDiv(13, s, 100), false);
    g_rs->fBtn = MakeUiFont(MulDiv(12, s, 100), false);
}
// ---------------------------------------------------------------- 绘制小工具

static void DrawLogoMark(HDC dc, int x, int y, int size) {
    static HICON s_logo = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        s_logo = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 48, 48,
                                   LR_DEFAULTCOLOR);
    }
    if (s_logo) {
        ui::DrawIcon22(dc, s_logo, x, y, size);
        return;
    }
    RECT rc{x, y, x + size, y + size};
    ui::RoundRect(dc, rc, RS(6), ui::Accent(), CLR_INVALID);
    RECT tr{x, y + RS(1), x + size, y + size};
    ui::Text(dc, L"Z", tr, RGB(255, 255, 255), g_rs->fName,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void DrawCopyMark(HDC dc, int x, int y, int size, COLORREF c, COLORREF bg) {
    int r = RS(2);
    RECT back{x + size / 5, y, x + size, y + size * 4 / 5};
    RECT front{x, y + size / 5, x + size * 4 / 5, y + size};
    ui::RoundRect(dc, back, r, CLR_INVALID, c);
    ui::RoundRect(dc, front, r, bg, c);
}

static void DrawCheckMark(HDC dc, int x, int y, int size, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, RS(2) > 0 ? RS(2) : 1, c);
    HGDIOBJ op = SelectObject(dc, pen);
    POINT pts[3] = {{x + size * 18 / 100, y + size * 52 / 100},
                    {x + size * 42 / 100, y + size * 74 / 100},
                    {x + size * 82 / 100, y + size * 26 / 100}};
    Polyline(dc, pts, 3);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void DrawCrossMark(HDC dc, int x, int y, int size, COLORREF c) {
    HPEN pen = CreatePen(PS_SOLID, RS(1) > 0 ? RS(1) : 1, c);
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, x, y, NULL);
    LineTo(dc, x + size, y + size);
    MoveToEx(dc, x + size, y, NULL);
    LineTo(dc, x, y + size);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// 细滚动条：默认弱化，滑过/滚动时加深
static void DrawThinBar(HDC dc, RECT rc, int contentH, int viewH, int scroll, bool hot) {
    if (contentH <= viewH || viewH <= 0) return;
    int w = RS(4);
    if (w < 3) w = 3;
    int x = rc.right - w;
    int h = rc.bottom - rc.top;
    if (h <= 0) return;
    int thumbH = MulDiv(h, viewH, contentH);
    if (thumbH < RS(18)) thumbH = RS(18);
    if (thumbH > h) thumbH = h;
    int maxScroll = contentH - viewH;
    int y = rc.top + (maxScroll > 0 ? MulDiv(h - thumbH, scroll, maxScroll) : 0);
    RECT t{x, y, x + w, y + thumbH};
    ui::RoundRect(dc, t, w / 2, hot ? kThumbHot : kThumb, CLR_INVALID);
}

static int MeasureWrapped(HDC dc, const std::wstring& s, HFONT f, int w) {
    if (s.empty() || w <= 0) return 0;
    HGDIOBJ old = SelectObject(dc, f);
    RECT rc{0, 0, w, 0};
    DrawTextW(dc, s.c_str(), (int)s.size(), &rc,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
    SelectObject(dc, old);
    return rc.bottom - rc.top;
}

// 单行高度（和 DrawTextW 换行用的一致）：裁剪按整行做，避免把一行字切一半
static int LineUnitOf(HDC dc, HFONT f) {
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    int u = tm.tmHeight + tm.tmExternalLeading;
    return u > 0 ? u : 20;
}

// 原文卡片右上角的「检测源语言 → 目标语言」展示
static std::wstring BuildLangDirectionLabel(const std::wstring& s, const std::wstring& activeLang) {
    std::wstring src = DetectSourceLangLabel(s);
    std::wstring dst = LangDisplayName(activeLang);
    return src + L"  →  " + dst;
}

static std::wstring ResultAllText() {
    std::wstring out;
    if (!g_rs) return out;
    for (const auto& r : g_rs->results) {
        if (r.text.empty()) continue;
        if (!out.empty()) out += L"\r\n\r\n";
        out += r.provider + L"\r\n" + r.text;
    }
    return out;
}

static void ResultCopy(int target) {
    if (!g_rs) return;
    std::wstring text;
    if (target == -2) text = ResultAllText();
    else if (target == -4) text = g_rs->original;
    else if (target >= 0 && target < (int)g_rs->results.size()) text = g_rs->results[target].text;
    if (text.empty()) return;

    if (OpenClipboard(g_rs->hwnd)) {
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            void* dst = GlobalLock(mem);
            if (dst) memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }
    g_rs->copied = target;
    g_rs->copiedUntil = GetTickCount() + 1400;
    SetTimer(g_rs->hwnd, 1, 1400, NULL);
    InvalidateRect(g_rs->hwnd, NULL, FALSE);
}
// ---------------------------------------------------------------- 布局

static bool IsResultPtInsideMonitor(POINT pt, int margin) {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) return true;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return true;
    return pt.x >= mi.rcMonitor.left + margin && pt.x < mi.rcMonitor.right - margin &&
           pt.y >= mi.rcMonitor.top + margin && pt.y < mi.rcMonitor.bottom - margin;
}

// 仅计算翻译结果窗宽高与内部卡片布局（不触发 SetWindowPos）
static void ResultComputeLayout() {
    if (!g_rs || !g_rs->hwnd) return;
    HDC dc = GetDC(g_rs->hwnd);
    const int pad = RS(20);
    const int titleH = RS(44);
    int maxW = (g_rs->work.right - g_rs->work.left) - RS(40);
    if (maxW < RS(320)) maxW = RS(320);
    g_rs->width = MulDiv(720, g_rs->scale, 100);
    if (g_rs->width > maxW) g_rs->width = maxW;
    const int w = g_rs->width;
    const int bodyW = w - pad * 2;

    // 标题栏右侧：关闭 +（多引擎时）复制全部 + 目标语言切换胶囊
    int btn = RS(28);
    g_rs->rcClose = RECT{w - pad - btn, (titleH - btn) / 2, w - pad, (titleH - btn) / 2 + btn};
    int caW = RS(86), caH = RS(26);
    bool showCopyAll = (!g_rs->pending && g_rs->results.size() > 1);
    if (showCopyAll) {
        g_rs->rcCopyAll = RECT{g_rs->rcClose.left - RS(6) - caW, (titleH - caH) / 2,
                               g_rs->rcClose.left - RS(6), (titleH - caH) / 2 + caH};
    } else {
        SetRectEmpty(&g_rs->rcCopyAll);
    }
    {
        std::wstring pillTxt =
            (g_rs->overrideLang.empty() ? L"自动 · " : L"译为：") +
            LangDisplayName(g_rs->activeLang) + L" ▾";
        HGDIOBJ oldF = SelectObject(dc, g_rs->fBtn);
        SIZE lsz{};
        GetTextExtentPoint32W(dc, pillTxt.c_str(), (int)pillTxt.size(), &lsz);
        SelectObject(dc, oldF);
        int lw = lsz.cx + RS(20);
        int rightEdge = (showCopyAll ? g_rs->rcCopyAll.left : g_rs->rcClose.left) - RS(6);
        g_rs->rcLangBtn = RECT{rightEdge - lw, (titleH - caH) / 2, rightEdge, (titleH - caH) / 2 + caH};
    }

    // ---- 原文卡片（高度自适应，超过 6 行内部滚动）
    const int srcPad = RS(12);
    const int srcHeadH = RS(18);
    const int srcInnerW = bodyW - srcPad * 2 - RS(28);
    int natural = MeasureWrapped(dc, g_rs->original, g_rs->fSrc, srcInnerW);
    int line = LineUnitOf(dc, g_rs->fSrc);
    if (natural < line) natural = line;
    g_rs->srcTextH = natural;
    // 最多 6 行，按整行裁剪（不会出现半行）
    g_rs->srcViewH = natural > line * 6 ? line * 6 : natural;
    if (g_rs->srcScroll > natural - g_rs->srcViewH) g_rs->srcScroll = natural - g_rs->srcViewH;
    if (g_rs->srcScroll < 0) g_rs->srcScroll = 0;
    int srcCardH = srcPad + srcHeadH + RS(8) + g_rs->srcViewH + srcPad;
    g_rs->rcSrcCard = RECT{pad, titleH + RS(6), pad + bodyW, titleH + RS(6) + srcCardH};
    g_rs->rcSrcCopy = RECT{g_rs->rcSrcCard.right - srcPad - RS(22), g_rs->rcSrcCard.top + RS(5),
                           g_rs->rcSrcCard.right - srcPad, g_rs->rcSrcCard.top + RS(27)};

    // ---- 结果卡片
    g_rs->rcCard.clear();
    g_rs->rcCopy.clear();
    g_rs->rcPanel.clear();
    g_rs->cardTextH.clear();
    g_rs->cardViewH.clear();
    if (g_rs->cardScroll.size() < g_rs->results.size())
        g_rs->cardScroll.resize(g_rs->results.size(), 0);

    const int cardPadX = RS(14), cardPadY = RS(12), panelPad = RS(10), gap = RS(10);
    const int headH = RS(26);
    const int copyW = RS(74), copyH = RS(26);
    int listTop = g_rs->rcSrcCard.bottom + RS(12);
    int y = listTop;

    for (size_t i = 0; i < g_rs->results.size(); ++i) {
        const TrResult& r = g_rs->results[i];
        const int innerW = bodyW - cardPadX * 2 - panelPad * 2;
        bool failed = r.text.empty();
        std::wstring body = failed ? (r.error.empty() ? std::wstring(L"（没有返回内容）")
                                                      : (L"翻译失败 · " + r.error))
                                   : r.text;
        HFONT bf = failed ? g_rs->fSrc : g_rs->fBody;
        int bodyLine = LineUnitOf(dc, bf);
        int th = MeasureWrapped(dc, body, bf, innerW);
        if (th < bodyLine) th = bodyLine;
        // 最多 8 行；裁剪按整行，避免把最后一行字切一半
        int cap = bodyLine * 8;
        int viewH = th > cap ? cap : (th / bodyLine) * bodyLine;
        if (viewH < bodyLine) viewH = bodyLine;
        int cardH = cardPadY + headH + RS(10) + panelPad * 2 + viewH + cardPadY;

        RECT card{pad, y, pad + bodyW, y + cardH};
        g_rs->rcCard.push_back(card);
        g_rs->rcCopy.push_back(RECT{card.right - cardPadX - copyW, card.top + cardPadY,
                                    card.right - cardPadX, card.top + cardPadY + copyH});
        RECT panel{card.left + cardPadX, card.top + cardPadY + headH + RS(10),
                   card.right - cardPadX,
                   card.top + cardPadY + headH + RS(10) + panelPad * 2 + viewH};
        g_rs->rcPanel.push_back(panel);
        g_rs->cardTextH.push_back(th);
        g_rs->cardViewH.push_back(viewH);
        if (g_rs->cardScroll[i] > th - viewH) g_rs->cardScroll[i] = th - viewH;
        if (g_rs->cardScroll[i] < 0) g_rs->cardScroll[i] = 0;
        y = card.bottom + gap;
    }

    int listH = y - listTop - (g_rs->results.empty() ? 0 : gap);
    if (g_rs->results.empty()) listH = RS(52);  // 还在翻译：占位一行，避免高度抖动

    int maxH = (g_rs->work.bottom - g_rs->work.top) - RS(40);
    int desired = listTop + listH + RS(16);
    int viewH = listH;
    if (desired > maxH) viewH = maxH - listTop - RS(16);
    if (viewH < RS(96)) viewH = RS(96);

    g_rs->rcList = RECT{pad, listTop, pad + bodyW, listTop + viewH};
    g_rs->contentH = listH;
    g_rs->maxScroll = listH > viewH ? listH - viewH : 0;
    if (g_rs->scrollY > g_rs->maxScroll) g_rs->scrollY = g_rs->maxScroll;
    if (g_rs->scrollY < 0) g_rs->scrollY = 0;

    g_rs->height = listTop + viewH + RS(16);
    ReleaseDC(g_rs->hwnd, dc);
}

static void ResultRebuild(bool recenter = false) {
    if (!g_rs || !g_rs->hwnd) return;
    RECT before{};
    bool hadBefore = !recenter && GetWindowRect(g_rs->hwnd, &before) && (before.right > before.left);
    if (hadBefore) {
        POINT mid{(before.left + before.right) / 2, (before.top + before.bottom) / 2};
        g_rs->work = MonitorWorkRect(mid);
    }

    ResultComputeLayout();
    const int w = g_rs->width;

    int px, py;
    if (hadBefore) {
        px = before.left;
        py = before.top;
        // 异步回包撑高窗口时，若底部超出当前屏工作区则适当上移，保持原水平位置不变
        if (py + g_rs->height > g_rs->work.bottom) py = g_rs->work.bottom - g_rs->height;
        if (py < g_rs->work.top) py = g_rs->work.top;
    } else {
        px = g_rs->work.left + ((g_rs->work.right - g_rs->work.left) - w) / 2;
        py = g_rs->work.top + ((g_rs->work.bottom - g_rs->work.top) - g_rs->height) / 2;
        if (px < g_rs->work.left) px = g_rs->work.left;
        if (px + w > g_rs->work.right) px = g_rs->work.right - w;
        if (py < g_rs->work.top) py = g_rs->work.top;
        if (py + g_rs->height > g_rs->work.bottom) py = g_rs->work.bottom - g_rs->height;
    }

    ZLog(L"[res] place at %d,%d %dx%d work=%ld,%ld-%ld,%ld list=%d..%d content=%d scroll=%d", px, py,
         w, g_rs->height, g_rs->work.left, g_rs->work.top, g_rs->work.right, g_rs->work.bottom,
         g_rs->rcList.top, g_rs->rcList.bottom, g_rs->contentH, g_rs->maxScroll);
    g_rs->inSizing = true;
    SetWindowPos(g_rs->hwnd, HWND_TOPMOST, px, py, w, g_rs->height, SWP_NOACTIVATE);
    g_rs->inSizing = false;
    InvalidateRect(g_rs->hwnd, NULL, FALSE);
}
// ---------------------------------------------------------------- 绘制

static void ResultPaintBody(HDC dc) {
    if (!g_rs) return;
    RECT rc;
    GetClientRect(g_rs->hwnd, &rc);
    const int pad = RS(20);
    const int titleH = RS(44);
    const bool copiedAll = g_rs->copied == -2;

    // 细边框，不要厚重阴影
    ui::RoundRect(dc, RECT{0, 0, rc.right, rc.bottom}, RS(10), ui::Bg(), kCardBorder);

    // ---- 标题栏
    DrawLogoMark(dc, pad, (titleH - RS(20)) / 2, RS(20));
    ui::TextLine(dc, L"翻译结果", pad + RS(28), (titleH - RS(22)) / 2, RS(180), RS(22), ui::Text(),
                 g_rs->fTitle);

    // 目标语言快速切换按钮
    {
        bool hov = (g_rs->hover == -5);
        bool manual = !g_rs->overrideLang.empty();
        RECT b = g_rs->rcLangBtn;
        COLORREF fill = (hov || manual) ? ui::AccentSoft() : ui::Panel();
        COLORREF bd = (hov || manual) ? ui::Accent() : kCardBorderHot;
        COLORREF tc = (hov || manual) ? ui::Accent() : ui::Text();
        ui::RoundRect(dc, b, RS(7), fill, bd);
        std::wstring pillTxt =
            (manual ? L"译为：" : L"自动 · ") + LangDisplayName(g_rs->activeLang) + L" ▾";
        ui::Text(dc, pillTxt, b, tc, g_rs->fBtn,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    if (!g_rs->pending && g_rs->results.size() > 1) {
        bool hov = g_rs->hover == -2;
        RECT b = g_rs->rcCopyAll;
        if (hov || copiedAll)
            ui::RoundRect(dc, b, RS(7), copiedAll ? ui::AccentSoft() : ui::Panel(),
                          copiedAll ? ui::Accent() : kCardBorder);
        COLORREF tc = copiedAll ? ui::Accent() : (hov ? ui::Accent() : ui::SubText());
        int gsz = RS(12);
        int gx = b.left + RS(10);
        int gy = b.top + (b.bottom - b.top - gsz) / 2;
        if (copiedAll) {
            DrawCheckMark(dc, gx, gy, gsz, tc);
            ui::TextLine(dc, L"已复制", gx + gsz + RS(5), b.top, RS(60), b.bottom - b.top, tc,
                         g_rs->fBtn);
        } else {
            DrawCopyMark(dc, gx, gy, gsz, tc, hov ? ui::Panel() : ui::Bg());
            ui::TextLine(dc, L"复制全部", gx + gsz + RS(5), b.top, RS(60), b.bottom - b.top, tc,
                         g_rs->fBtn);
        }
    }

    {
        bool hov = g_rs->hover == -3;
        RECT b = g_rs->rcClose;
        if (hov) ui::RoundRect(dc, b, RS(7), ui::Panel(), CLR_INVALID);
        COLORREF cc = hov ? ui::Text() : ui::SubText();
        DrawCrossMark(dc, b.left + RS(9), b.top + RS(9), RS(10), cc);
    }

    // ---- 原文卡片
    ui::RoundRect(dc, g_rs->rcSrcCard, RS(8), ui::Panel(), CLR_INVALID);
    {
        RECT c = g_rs->rcSrcCard;
        ui::TextLine(dc, L"原文", c.left + RS(12), c.top + RS(10), RS(60), RS(16), ui::SubText(),
                     g_rs->fTagB);
        std::wstring lang = BuildLangDirectionLabel(g_rs->original, g_rs->activeLang);
        RECT lr{c.right - RS(12) - RS(210) - RS(26), c.top + RS(10), c.right - RS(12) - RS(26),
                c.top + RS(26)};
        ui::Text(dc, lang, lr, ui::SubText(), g_rs->fTag,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        bool hov = g_rs->hover == -4;
        RECT b = g_rs->rcSrcCopy;
        ui::RoundRect(dc, b, RS(6), hov ? ui::Bg() : ui::Panel(), hov ? kCardBorderHot : CLR_INVALID);
        DrawCopyMark(dc, b.left + RS(6), b.top + RS(6), RS(10), hov ? ui::Accent() : ui::SubText(),
                     hov ? ui::Bg() : ui::Panel());

        RECT tr{c.left + RS(12), c.top + RS(34), c.right - RS(12), c.top + RS(34) + g_rs->srcViewH};
        int saved = SaveDC(dc);
        IntersectClipRect(dc, tr.left, tr.top, tr.right, tr.bottom);
        RECT draw = tr;
        draw.top -= g_rs->srcScroll;
        draw.bottom = draw.top + g_rs->srcTextH + RS(40);
        ui::Text(dc, g_rs->original, draw, ui::Text(), g_rs->fSrc,
                 DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        RestoreDC(dc, saved);
        DrawThinBar(dc, RECT{c.right - RS(10), tr.top, c.right - RS(6), tr.bottom}, g_rs->srcTextH,
                    g_rs->srcViewH, g_rs->srcScroll, g_rs->barHot);
    }

    // ---- 结果列表（整列滚动，卡片内部只有超长译文才滚动）
    if (g_rs->pending && g_rs->results.empty()) {
        std::wstring waitTxt = L"正在译为" + LangDisplayName(g_rs->activeLang) + L"…";
        ui::TextLine(dc, waitTxt, pad, g_rs->rcList.top + RS(14), RS(320), RS(22),
                     ui::SubText(), g_rs->fSrc);
    }

    int saved = SaveDC(dc);
    IntersectClipRect(dc, g_rs->rcList.left, g_rs->rcList.top, g_rs->rcList.right,
                      g_rs->rcList.bottom);
    for (size_t i = 0; i < g_rs->results.size(); ++i) {
        const TrResult& r = g_rs->results[i];
        RECT card = g_rs->rcCard[i];
        OffsetRect(&card, 0, -g_rs->scrollY);
        if (card.bottom < g_rs->rcList.top - RS(24) || card.top > g_rs->rcList.bottom + RS(24))
            continue;
        const bool cardHot = g_rs->cardHover == (int)i;
        const bool failed = r.text.empty();

        ui::RoundRect(dc, card, RS(10), cardHot ? RGB(252, 252, 253) : ui::Bg(),
                      cardHot ? kCardBorderHot : kCardBorder);

        int headW = card.right - card.left - RS(170);
        ui::TextLine(dc, r.provider, card.left + RS(14), card.top + RS(12), headW, RS(26),
                     ui::Text(), g_rs->fName);

        RECT cb = g_rs->rcCopy[i];
        OffsetRect(&cb, 0, -g_rs->scrollY);
        {
            bool hov = g_rs->hover == (int)i;
            bool done = g_rs->copied == (int)i;
            if (!failed && r.ms > 0) {
                RECT meta{cb.left - RS(96), cb.top, cb.left - RS(8), cb.bottom};
                ui::Text(dc, std::to_wstring(r.ms) + L" ms", meta, ui::SubText(), g_rs->fMeta,
                         DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            bool press = g_rs->pressed == (int)i;
            COLORREF cbBg = done ? ui::AccentSoft() : (press ? ui::Panel() : ui::Bg());
            ui::RoundRect(dc, cb, RS(7), cbBg,
                          done ? ui::Accent() : (hov ? ui::Accent() : ui::Border()));
            COLORREF tc = done ? ui::Accent() : (hov ? ui::Accent() : ui::Text());
            COLORREF gbg = cbBg;
            int gsz = RS(12);
            int gx = cb.left + RS(12);
            int gy = cb.top + (cb.bottom - cb.top - gsz) / 2;
            if (done) {
                DrawCheckMark(dc, gx, gy, gsz, tc);
                ui::TextLine(dc, L"已复制", gx + gsz + RS(5), cb.top, RS(60), cb.bottom - cb.top, tc,
                             g_rs->fBtn);
            } else {
                DrawCopyMark(dc, gx, gy, gsz, tc, gbg);
                ui::TextLine(dc, L"复制", gx + gsz + RS(5), cb.top, RS(60), cb.bottom - cb.top, tc,
                             g_rs->fBtn);
            }
        }

        RECT panel = g_rs->rcPanel[i];
        OffsetRect(&panel, 0, -g_rs->scrollY);
        ui::RoundRect(dc, panel, RS(6), ui::Panel(), CLR_INVALID);
        std::wstring body = failed ? (r.error.empty() ? std::wstring(L"（没有返回内容）")
                                                     : (L"翻译失败 · " + r.error))
                                   : r.text;
        RECT tr{panel.left + RS(10), panel.top + RS(10), panel.right - RS(10),
                panel.top + RS(10) + g_rs->cardViewH[i]};
        int s2 = SaveDC(dc);
        IntersectClipRect(dc, tr.left, tr.top, tr.right, tr.bottom);
        RECT draw = tr;
        draw.top -= g_rs->cardScroll[i];
        draw.bottom = draw.top + g_rs->cardTextH[i] + RS(40);
        ui::Text(dc, body, draw, failed ? kErrText : ui::Text(), failed ? g_rs->fSrc : g_rs->fBody,
                 DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        RestoreDC(dc, s2);
        DrawThinBar(dc, RECT{panel.right - RS(10), tr.top, panel.right - RS(6), tr.bottom},
                    g_rs->cardTextH[i], g_rs->cardViewH[i], g_rs->cardScroll[i], g_rs->barHot);
    }
    RestoreDC(dc, saved);

    // 列表上下淡出：滚动中卡片不会硬切在窗口边缘
    if (g_rs->maxScroll > 0) {
        auto fade = [&](int y0, int y1, BYTE a0, BYTE a1) {
            TRIVERTEX v[2] = {{g_rs->rcList.left, y0, 0xFFFF, 0xFFFF, 0xFFFF, (COLOR16)(a0 << 8)},
                              {g_rs->rcList.right, y1, 0xFFFF, 0xFFFF, 0xFFFF, (COLOR16)(a1 << 8)}};
            GRADIENT_RECT gr{0, 1};
            GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
        };
        int fh = RS(14);
        if (g_rs->scrollY > 0) fade(g_rs->rcList.top, g_rs->rcList.top + fh, 255, 0);
        if (g_rs->scrollY < g_rs->maxScroll) fade(g_rs->rcList.bottom - fh, g_rs->rcList.bottom, 0, 255);
    }

    // 整列滚动条（贴右侧留白，默认弱化）
    if (g_rs->maxScroll > 0) {
        int listH = g_rs->rcList.bottom - g_rs->rcList.top;
        RECT bar{rc.right - pad + RS(8), g_rs->rcList.top, rc.right - pad + RS(12),
                 g_rs->rcList.bottom};
        DrawThinBar(dc, bar, g_rs->contentH, listH, g_rs->scrollY, g_rs->barHot);
        int thumbH = MulDiv(listH, listH, g_rs->contentH);
        if (thumbH < RS(18)) thumbH = RS(18);
        if (thumbH > listH) thumbH = listH;
        int ty = g_rs->rcList.top + MulDiv(listH - thumbH, g_rs->scrollY, g_rs->maxScroll);
        g_rs->rcBarThumb = RECT{bar.left, ty, bar.right, ty + thumbH};
    } else {
        SetRectEmpty(&g_rs->rcBarThumb);
    }
}
// ---------------------------------------------------------------- 窗口过程

// 按钮命中：-100 无 / -5 切换语言 / -3 关闭 / -2 复制全部 / -4 复制原文 / >=0 卡片复制
static int ResultHitAt(POINT pt) {
    if (!g_rs) return -100;
    if (PtInRect(&g_rs->rcClose, pt)) return -3;
    if (PtInRect(&g_rs->rcLangBtn, pt)) return -5;
    if (!g_rs->pending && g_rs->results.size() > 1 && PtInRect(&g_rs->rcCopyAll, pt)) return -2;
    if (PtInRect(&g_rs->rcSrcCopy, pt)) return -4;
    POINT cp{pt.x, pt.y + g_rs->scrollY};
    for (size_t i = 0; i < g_rs->rcCopy.size(); ++i)
        if (PtInRect(&g_rs->rcCopy[i], cp)) return (int)i;
    return -100;
}

static void ShowResultLangMenu(HWND hwnd) {
    if (!g_rs) return;
    std::vector<ui::DropdownItem> items;
    {
        ui::DropdownItem autoIt;
        autoIt.id = 1000;
        autoIt.label = L"自动双向互译";
        autoIt.sub = LangDisplayName(g_cfg.targetLang) + L"↔" + LangDisplayName(g_cfg.cnTargetLang);
        autoIt.checked = g_rs->overrideLang.empty();
        items.push_back(std::move(autoIt));
    }
    {
        ui::DropdownItem sep;
        sep.id = -1;
        items.push_back(sep);
    }

    const auto& langs = SupportedTargetLangs();
    for (size_t i = 0; i < langs.size(); ++i) {
        ui::DropdownItem it;
        it.id = 1001 + (int)i;
        it.label = langs[i].label;
        it.checked = !g_rs->overrideLang.empty() &&
                     _wcsicmp(g_rs->overrideLang.c_str(), langs[i].code) == 0;
        items.push_back(std::move(it));
    }

    POINT p0{g_rs->rcLangBtn.left, g_rs->rcLangBtn.top};
    POINT p1{g_rs->rcLangBtn.right, g_rs->rcLangBtn.bottom};
    ClientToScreen(hwnd, &p0);
    ClientToScreen(hwnd, &p1);
    RECT anchorScreen{p0.x, p0.y, p1.x, p1.y};
    RECT winScreen{};
    GetWindowRect(hwnd, &winScreen);

    int cmd = ui::ShowDropdownPopup(hwnd, anchorScreen, items, g_rs->scale, &winScreen);
    if (!g_rs || cmd < 1000) return;

    std::wstring nextOverride = (cmd == 1000) ? L"" : langs[cmd - 1001].code;
    std::wstring nextActive = ResolveTargetLang(g_rs->original, nextOverride);
    bool hadError = false;
    for (const auto& r : g_rs->results)
        if (r.text.empty()) hadError = true;

    g_rs->overrideLang = nextOverride;
    if (nextActive == g_rs->activeLang && !g_rs->pending && !hadError) {
        ResultRebuild();
        return;
    }

    g_rs->activeLang = nextActive;
    uint64_t rid = ++g_rs->reqId;
    g_rs->pending = true;
    g_rs->results.clear();
    g_rs->scrollY = 0;
    ResultRebuild();

    std::wstring orig = g_rs->original;
    bool selMode = g_rs->selectMode;
    HWND target = hwnd;
    std::thread([orig, nextActive, selMode, target, rid]() {
        std::vector<TrResult> res =
            selMode ? TranslateSelectAll(orig, nextActive) : TranslateAll(orig, nextActive);
        TranslatePayload* tp = new TranslatePayload();
        tp->original = orig;
        tp->results = std::move(res);
        tp->target = target;
        tp->reqId = rid;
        if (g_msgWnd && IsWindow(g_msgWnd))
            PostMessageW(g_msgWnd, WM_ZPIX_TRANSLATE_DONE, 0, (LPARAM)tp);
        else
            delete tp;
    }).detach();
}

static int ResultCardAt(POINT pt) {
    if (!g_rs || !PtInRect(&g_rs->rcList, pt)) return -1;
    POINT cp{pt.x, pt.y + g_rs->scrollY};
    for (size_t i = 0; i < g_rs->rcCard.size(); ++i)
        if (PtInRect(&g_rs->rcCard[i], cp)) return (int)i;
    return -1;
}

static LRESULT CALLBACK ResultProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_rs = new ResultState();
        g_rs->hwnd = hwnd;
        HDC dc = GetDC(hwnd);
        int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
        if (dpi <= 0) dpi = 96;
        g_rs->scale = MulDiv(dpi, 100, 96);
        ResultApplyScale();
        SetFocus(hwnd);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1,
                                            rc.bottom > 0 ? rc.bottom : 1);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH bg = CreateSolidBrush(ui::Bg());
        FillRect(mem, &rc, bg);
        DeleteObject(bg);
        ResultPaintBody(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: {
        // 自己管光标：按钮上用手型，其他地方箭头（否则 DefWindowProc 会把光标重置回箭头）
        if (LOWORD(lp) != HTCLIENT) break;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        SetCursor(LoadCursorW(NULL, ResultHitAt(pt) != -100 ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g_rs->winDragging) {
            POINT sp{};
            GetCursorPos(&sp);
            int curScale = MonitorScaleOf(sp);
            if (curScale != g_rs->scale && IsResultPtInsideMonitor(sp, 6)) {
                g_rs->inSizing = true;
                g_rs->scale = curScale;
                g_rs->work = MonitorWorkRect(sp);
                ResultApplyScale();
                ResultComputeLayout();
                g_rs->dragOffX = (int)std::lround(g_rs->dragRx * g_rs->width);
                g_rs->dragOffY = (int)std::lround(g_rs->dragRy * g_rs->height);
                int nx = sp.x - g_rs->dragOffX;
                int ny = sp.y - g_rs->dragOffY;
                SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, g_rs->width, g_rs->height, SWP_NOACTIVATE);
                g_rs->inSizing = false;
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateWindow(hwnd);
            } else {
                SetWindowPos(hwnd, HWND_TOPMOST, sp.x - g_rs->dragOffX, sp.y - g_rs->dragOffY, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return 0;
        }
        if (g_rs->dragBar && g_rs->maxScroll > 0) {
            int h = g_rs->rcList.bottom - g_rs->rcList.top;
            int thumbH = g_rs->rcBarThumb.bottom - g_rs->rcBarThumb.top;
            int span = h - thumbH;
            if (span > 0) {
                int v = g_rs->dragOriginScroll +
                        MulDiv(pt.y - g_rs->dragOriginY, g_rs->maxScroll, span);
                if (v < 0) v = 0;
                if (v > g_rs->maxScroll) v = g_rs->maxScroll;
                g_rs->scrollY = v;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        int hit = ResultHitAt(pt);
        int cardHit = ResultCardAt(pt);

        if (hit != g_rs->hover || cardHit != g_rs->cardHover) {
            g_rs->hover = hit;
            g_rs->cardHover = cardHit;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        bool overList = PtInRect(&g_rs->rcList, pt) || PtInRect(&g_rs->rcSrcCard, pt);
        if (overList && !g_rs->barHot) {
            g_rs->barHot = true;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (!overList && g_rs->barHot && GetTickCount() > g_rs->barUntil) {
            g_rs->barHot = false;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (!g_rs->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_rs->tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_rs->tracking = false;
        g_rs->pressed = -100;
        if (GetTickCount() > g_rs->barUntil) g_rs->barHot = false;
        g_rs->hover = -100;
        g_rs->cardHover = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_rs->barHot = true;
        g_rs->barUntil = GetTickCount() + 1200;
        if (!IsRectEmpty(&g_rs->rcBarThumb) && PtInRect(&g_rs->rcBarThumb, pt)) {
            g_rs->dragBar = true;
            g_rs->dragOriginY = pt.y;
            g_rs->dragOriginScroll = g_rs->scrollY;
            SetCapture(hwnd);
            return 0;
        }
        int hit = ResultHitAt(pt);
        if (pt.y < RS(44) && hit == -100) {
            POINT sp{};
            GetCursorPos(&sp);
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            int ww = (wr.right - wr.left) > 0 ? (wr.right - wr.left) : 1;
            int wh = (wr.bottom - wr.top) > 0 ? (wr.bottom - wr.top) : 1;
            g_rs->winDragging = true;
            g_rs->dragOffX = sp.x - wr.left;
            g_rs->dragOffY = sp.y - wr.top;
            g_rs->dragRx = (double)g_rs->dragOffX / (double)ww;
            g_rs->dragRy = (double)g_rs->dragOffY / (double)wh;
            if (g_rs->dragRx < 0.0) g_rs->dragRx = 0.0;
            if (g_rs->dragRx > 1.0) g_rs->dragRx = 1.0;
            if (g_rs->dragRy < 0.0) g_rs->dragRy = 0.0;
            if (g_rs->dragRy > 1.0) g_rs->dragRy = 1.0;
            SetCapture(hwnd);
            return 0;
        }
        g_rs->pressed = hit;
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_rs->winDragging) {
            g_rs->winDragging = false;
            g_rs->pressed = -100;
            ReleaseCapture();
            RECT wr{};
            if (GetWindowRect(hwnd, &wr) && wr.right > wr.left) {
                POINT mid{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
                int settleScale = MonitorScaleOf(mid);
                if (settleScale != g_rs->scale) {
                    g_rs->inSizing = true;
                    g_rs->scale = settleScale;
                    g_rs->work = MonitorWorkRect(mid);
                    ResultApplyScale();
                    ResultComputeLayout();
                    int nx = mid.x - g_rs->width / 2;
                    int ny = wr.top;
                    SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, g_rs->width, g_rs->height, SWP_NOACTIVATE);
                    g_rs->inSizing = false;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        if (g_rs->dragBar) {
            g_rs->dragBar = false;
            ReleaseCapture();
            return 0;
        }
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = ResultHitAt(pt);
        if (hit == -3) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (hit == -5) {
            g_rs->pressed = -100;
            g_rs->hover = -100;
            InvalidateRect(hwnd, NULL, FALSE);
            ShowResultLangMenu(hwnd);
            return 0;
        }
        if (hit != -100) ResultCopy(hit);
        if (g_rs->pressed != -100) {
            g_rs->pressed = -100;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int step = MulDiv(RS(64), delta, WHEEL_DELTA);
        g_rs->barHot = true;
        g_rs->barUntil = GetTickCount() + 1200;

        for (size_t i = 0; i < g_rs->rcPanel.size(); ++i) {
            RECT panel = g_rs->rcPanel[i];
            OffsetRect(&panel, 0, -g_rs->scrollY);
            if (PtInRect(&panel, pt) && g_rs->cardTextH[i] > g_rs->cardViewH[i]) {
                int maxS = g_rs->cardTextH[i] - g_rs->cardViewH[i];
                int v = g_rs->cardScroll[i] - step;
                if (v < 0) v = 0;
                if (v > maxS) v = maxS;
                g_rs->cardScroll[i] = v;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        if (PtInRect(&g_rs->rcSrcCard, pt) && g_rs->srcTextH > g_rs->srcViewH) {
            int maxS = g_rs->srcTextH - g_rs->srcViewH;
            int v = g_rs->srcScroll - step;
            if (v < 0) v = 0;
            if (v > maxS) v = maxS;
            g_rs->srcScroll = v;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_rs->maxScroll > 0) {
            int v = g_rs->scrollY - step;
            if (v < 0) v = 0;
            if (v > g_rs->maxScroll) v = g_rs->maxScroll;
            g_rs->scrollY = v;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            if (GetTickCount() >= g_rs->copiedUntil) {
                g_rs->copied = -100;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DPICHANGED: {
        if (!g_rs || g_rs->winDragging || g_rs->inSizing) return 0;
        UINT dpi = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
        int s2 = dpi ? MulDiv((int)dpi, 100, 96) : g_rs->scale;
        if (s2 < 100) s2 = 100;
        const RECT* sug = (const RECT*)lp;
        if (sug) {
            POINT mid{(sug->left + sug->right) / 2, (sug->top + sug->bottom) / 2};
            g_rs->work = MonitorWorkRect(mid);
        }
        if (s2 != g_rs->scale) {
            g_rs->scale = s2;
            ResultApplyScale();
        }
        ResultComputeLayout();
        int nx = sug ? sug->left : (g_rs->work.left + (g_rs->work.right - g_rs->work.left - g_rs->width) / 2);
        int ny = sug ? sug->top : (g_rs->work.top + (g_rs->work.bottom - g_rs->work.top - g_rs->height) / 2);
        g_rs->inSizing = true;
        SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, g_rs->width, g_rs->height, SWP_NOACTIVATE);
        g_rs->inSizing = false;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_rs) {
            HFONT* fonts[] = {&g_rs->fTitle, &g_rs->fName, &g_rs->fTag, &g_rs->fTagB,
                              &g_rs->fMeta,  &g_rs->fBody, &g_rs->fSrc, &g_rs->fBtn};
            for (HFONT* f : fonts)
                if (*f) DeleteObject(*f);
            delete g_rs;
            g_rs = nullptr;
        }
        g_resultWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void EnableRoundedCorners(HWND hwnd) {
    // Win11 圆角 + 系统阴影；Win10 上失败也无副作用
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

static void EnsureResultClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ResultProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"ZpixResult";
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);
    done = true;
}

HWND ShowResultWindow(const std::wstring& original, bool selectMode) {
    EnsureResultClass();
    if (g_rs) {
        DestroyWindow(g_rs->hwnd);
        g_rs = nullptr;
    }
    // 选区中心（没有的话退到鼠标位置）决定落在哪块屏
    POINT anchor = g_anchor;
    if (anchor.x == INT_MIN) {
        GetCursorPos(&anchor);
        ZLog(L"[res] anchor fallback cursor=%ld,%ld", anchor.x, anchor.y);
    }
    RECT work{};
    int mscale = 100;
    MonitorOfWorkArea(anchor, &work, &mscale);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, L"ZpixResult", L"Zpix 翻译",
                                WS_POPUP | WS_CLIPCHILDREN, 0, 0, 480, 320, NULL, NULL,
                                GetModuleHandleW(NULL), NULL);
    if (!hwnd) return NULL;
    g_rs->work = work;
    g_rs->scale = mscale;
    ResultApplyScale();
    int maxW = (work.right - work.left) - RS(40);
    g_rs->width = MulDiv(720, g_rs->scale, 100);
    if (maxW > RS(360) && g_rs->width > maxW) g_rs->width = maxW;
    ZLog(L"[res] width=%d", g_rs->width);
    g_rs->original = original;
    g_rs->selectMode = selectMode;
    g_rs->overrideLang.clear();
    g_rs->activeLang = ResolveTargetLang(original);
    g_rs->reqId = 1;
    g_rs->pending = true;
    ResultRebuild(true);
    EnableRoundedCorners(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    g_resultWnd = hwnd;
    return hwnd;
}

uint64_t CurrentResultReqId() { return g_rs ? g_rs->reqId : 0; }

void SetResultPayload(HWND hwnd, const std::wstring& original, const std::vector<TrResult>& results,
                      uint64_t reqId) {
    if (!g_rs || g_rs->hwnd != hwnd) return;
    if (reqId != 0 && reqId != g_rs->reqId) return;
    g_rs->original = original;
    g_rs->results = results;
    g_rs->pending = false;
    g_rs->scrollY = 0;
    g_rs->cardScroll.assign(results.size(), 0);
    ResultRebuild();
}

// ---------------------------------------------------------------- actions

static std::mutex g_saveMu;

static bool SaveShotToDailyArchive(const Bitmap32& img, const std::wstring& rootDir,
                                   std::wstring* outPath, std::wstring* outDateDir) {
    if (img.empty()) return false;
    std::lock_guard<std::mutex> lk(g_saveMu);
    std::wstring dateDir;
    std::wstring path = BuildDailyShotPath(rootDir, &dateDir);
    bool ok = SavePngFile(img, path);
    if (outPath) *outPath = path;
    if (outDateDir) *outDateDir = dateDir;
    return ok;
}

static void AutoArchiveShotAsync(const Bitmap32& img) {
    if (!g_cfg.autoSaveShot || img.empty()) return;
    std::wstring root = g_cfg.saveDir;
    Bitmap32 copy = img;
    std::thread([copy = std::move(copy), root]() {
        std::wstring path, dateDir;
        bool ok = SaveShotToDailyArchive(copy, root, &path, &dateDir);
        ZLog(L"[save] auto-archive ok=%d path=%ls", (int)ok, path.c_str());
    }).detach();
}

static void LaunchOcrAsync(const Bitmap32& img, bool translate) {
    static std::atomic<uint64_t> tmpSeq{0};
    Bitmap32 copy = img;
    uint64_t id = ++tmpSeq;
    ShowToast(L"正在识别文字…");
    std::thread([copy = std::move(copy), translate, id]() {
        std::wstring tmp = DataDir() + L"ocr_tmp_" + std::to_wstring(id) + L".png";
        std::wstring text;
        if (SavePngFile(copy, tmp)) {
            text = OcrImageFile(tmp);
            DeleteFileW(tmp.c_str());
        }
        OcrPayload* payload = new OcrPayload();
        payload->text = std::move(text);
        payload->translate = translate;
        if (g_msgWnd && IsWindow(g_msgWnd)) {
            PostMessageW(g_msgWnd, WM_ZPIX_OCR_DONE, 0, (LPARAM)payload);
        } else {
            delete payload;
        }
    }).detach();
}

void HandleOcrDone(OcrPayload* payload) {
    if (!payload) return;
    std::wstring text = std::move(payload->text);
    bool translate = payload->translate;
    delete payload;

    if (text.empty()) {
        ShowToast(L"没识别到文字，请把选区拉大一点再试");
        return;
    }
    if (!translate) {
        CopyTextToClipboard(g_msgWnd, text);
        ShowToast(L"已复制文字（" + std::to_wstring(text.size()) + L" 字）");
        return;
    }

    int enabled = 0;
    for (const auto& p : g_cfg.providers)
        if (p.enabled) ++enabled;
    if (enabled == 0) {
        ShowToast(L"没有启用任何翻译厂家");
        return;
    }
    ShowToast(L"正在用 " + std::to_wstring(enabled) + L" 个厂家翻译…");
    HWND result = ShowResultWindow(text, false);
    uint64_t rid = CurrentResultReqId();
    std::thread([text, result, rid]() {
        std::vector<TrResult> res = TranslateAll(text);
        TranslatePayload* tp = new TranslatePayload();
        tp->original = text;
        tp->results = res;
        tp->target = result;
        tp->reqId = rid;
        if (g_msgWnd && IsWindow(g_msgWnd))
            PostMessageW(g_msgWnd, WM_ZPIX_TRANSLATE_DONE, 0, (LPARAM)tp);
        else
            delete tp;
    }).detach();
}

bool DoShotAction(ShotAction action, const Bitmap32& img, HWND owner) {
    switch (action) {
    case ShotAction::CopyImage:
        CopyToClipboard(owner, img);
        AutoArchiveShotAsync(img);
        ShowToast(g_cfg.autoSaveShot ? L"已复制到剪贴板（已自动留存）" : L"已复制到剪贴板");
        return true;
    case ShotAction::Pin:
        CopyToClipboard(owner, img);
        AutoArchiveShotAsync(img);
        {
            // 贴图落在鼠标所在屏的正中间（同理，别用虚拟桌面中心）
            POINT cur{};
            GetCursorPos(&cur);
            RECT work = MonitorWorkRect(cur);
            POINT pt{work.left + (work.right - work.left - img.w) / 2,
                     work.top + (work.bottom - work.top - img.h) / 2};
            if (pt.x < work.left + 40) pt.x = work.left + 40;
            if (pt.y < work.top + 40) pt.y = work.top + 40;
            CreatePinFromBitmap(img, pt);
        }
        return true;
    case ShotAction::Save: {
        std::wstring path, dateDir;
        bool ok = SaveShotToDailyArchive(img, g_cfg.saveDir, &path, &dateDir);
        ZLog(L"[save] download ok=%d path=%ls", (int)ok, path.c_str());
        if (ok) {
            ShowToast(L"已保存：" + path);
            if (g_cfg.openFolderAfterSave) {
                OpenFolderAndSelectFile(path, dateDir);
            }
        } else {
            ShowToast(L"保存失败，请检查保存目录权限");
        }
        return ok;
    }
    case ShotAction::CopyText: {
        AutoArchiveShotAsync(img);
        LaunchOcrAsync(img, false);
        return true;
    }
    case ShotAction::Translate: {
        int enabled = 0;
        for (const auto& p : g_cfg.providers)
            if (p.enabled) ++enabled;
        if (enabled == 0) {
            ShowToast(L"没有启用任何翻译厂家");
            return false;
        }
        AutoArchiveShotAsync(img);
        LaunchOcrAsync(img, true);
        return true;
    }
    case ShotAction::ImageSearch: {
        std::wstring key = g_cfg.imgbbKey;
        while (!key.empty() && (key.front() == L' ' || key.front() == L'\t' ||
                                key.front() == L'\r' || key.front() == L'\n'))
            key.erase(key.begin());
        while (!key.empty() && (key.back() == L' ' || key.back() == L'\t' ||
                                key.back() == L'\r' || key.back() == L'\n'))
            key.pop_back();
        if (key.empty()) {
            ShowSettingsWindow(g_msgWnd, SETTINGS_TAB_IMGSEARCH);
            ShowToast(L"识图尚未配置 Key，请点击「免费获取API Key」创建后填入");
            return false;
        }
        AutoArchiveShotAsync(img);
        int engine = g_cfg.imageSearchEngine;
        std::wstring engLabel = img_search::EngineLabel(engine);
        ShowToast(L"正在上传图片并通过 " + engLabel + L" 识图…");
        std::thread([img, engine, key]() {
            img_search::SearchResult sr;
            bool ok = img_search::UploadAndBuildSearchUrl(img, engine, key, sr);
            ImgSearchDonePayload* p = new ImgSearchDonePayload();
            p->ok = ok;
            p->engineName = sr.engineName;
            p->targetUrl = sr.targetUrl;
            p->error = sr.error;
            if (!g_msgWnd || !PostMessageW(g_msgWnd, WM_ZPIX_IMGSEARCH_DONE, 0, (LPARAM)p)) {
                delete p;
            }
        }).detach();
        return true;
    }
    }
    return false;
}

void HandleImgSearchDone(ImgSearchDonePayload* payload) {
    if (!payload) return;
    if (payload->ok && !payload->targetUrl.empty()) {
        ZLog(L"[img-search] open browser engine=%ls url=%ls", payload->engineName.c_str(),
             payload->targetUrl.c_str());
        ShellExecuteW(NULL, L"open", payload->targetUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
        ShowToast(L"已在浏览器打开 " + payload->engineName + L" 识图结果");
    } else {
        std::wstring msg = payload->error.empty() ? L"上传失败，请检查网络或 API Key" : payload->error;
        ZLog(L"[img-search] failed err=%ls", msg.c_str());
        ShowToast(L"识图失败：" + msg);
    }
    delete payload;
}

// ---------------------------------------------------------------- toast

static HWND g_toastWnd = nullptr;

struct ToastState {
    std::wstring text;
};

static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ToastState* st = (ToastState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        st = (ToastState*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        SetTimer(hwnd, 1, 1800, NULL);
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(32, 33, 36));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        HFONT font = MakeUiFont(15);
        HGDIOBJ of = SelectObject(dc, font);
        RECT tr = rc;
        InflateRect(&tr, -14, 0);
        DrawTextW(dc, st ? st->text.c_str() : L"", -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (g_toastWnd == hwnd) g_toastWnd = nullptr;
        if (st) delete st;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowToast(const std::wstring& text) {
    static bool cls = false;
    if (!cls) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ToastProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"ZpixToast";
        RegisterClassExW(&wc);
        cls = true;
    }

    if (g_toastWnd && IsWindow(g_toastWnd)) {
        DestroyWindow(g_toastWnd);
        g_toastWnd = nullptr;
    }

    HDC dc = GetDC(NULL);
    HFONT font = MakeUiFont(15);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, of);
    DeleteObject(font);
    ReleaseDC(NULL, dc);

    // 提示条居中在"鼠标所在的那块屏"，不是虚拟桌面中间（那是两块屏的缝）
    POINT cur{};
    GetCursorPos(&cur);
    RECT work = MonitorWorkRect(cur);
    int maxW = (work.right - work.left) - 80;
    if (maxW < 240) maxW = 240;
    int w = sz.cx + 44;
    if (w > maxW) w = maxW;
    int h = 40;
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.bottom - 160;

    ToastState* st = new ToastState();
    st->text = text;

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"ZpixToast",
                                L"", WS_POPUP, x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), st);
    g_toastWnd = hwnd;
    EnableRoundedCorners(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
}
