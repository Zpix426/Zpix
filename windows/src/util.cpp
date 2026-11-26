#include "util.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <mutex>

using namespace Gdiplus;

static ULONG_PTR g_gdiplusToken = 0;

RECT GetVirtualScreenRect() {
    RECT r;
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}

// ---------------------------------------------------------------- 显示器选择
// 多屏定位的统一入口：只挑"一块"显示器，不会拿虚拟桌面大矩形去居中
// （两块屏横排时虚拟桌面中心正好在缝上，窗口就会一边一半）

typedef HRESULT(WINAPI* PFN_GetDpiForMon)(HMONITOR, int, UINT*, UINT*);

static int DpiOfMonitorRaw(HMONITOR mon) {
    static PFN_GetDpiForMon pfn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) pfn = (PFN_GetDpiForMon)GetProcAddress(shcore, "GetDpiForMonitor");
    }
    UINT x = 0, y = 0;
    if (pfn && mon && SUCCEEDED(pfn(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && y) return (int)y;
    return 0;
}

static RECT FallbackWorkRect() {
    RECT r{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0) && r.right > r.left && r.bottom > r.top)
        return r;
    r = GetVirtualScreenRect();
    return r;
}

struct MonPickCtx {
    POINT pt;
    bool primaryOnly;
    HMONITOR best;
    RECT bestWork;
    long long bestDist;
};

static BOOL CALLBACK PickMonitorProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    MonPickCtx* c = (MonPickCtx*)lp;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    bool primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    if (c->primaryOnly) {
        if (primary) {
            c->best = mon;
            c->bestWork = mi.rcWork;
            return FALSE;
        }
        return TRUE;
    }
    if (PtInRect(&mi.rcMonitor, c->pt)) {  // 命中：直接用这块屏
        c->best = mon;
        c->bestWork = mi.rcWork;
        return FALSE;
    }
    long long dx = 0, dy = 0;
    if (c->pt.x < mi.rcMonitor.left) dx = mi.rcMonitor.left - c->pt.x;
    else if (c->pt.x >= mi.rcMonitor.right) dx = c->pt.x - (mi.rcMonitor.right - 1);
    if (c->pt.y < mi.rcMonitor.top) dy = mi.rcMonitor.top - c->pt.y;
    else if (c->pt.y >= mi.rcMonitor.bottom) dy = c->pt.y - (mi.rcMonitor.bottom - 1);
    long long d = dx * dx + dy * dy;
    if (!c->best || d < c->bestDist) {  // 落在缝上/屏外：取最近那块
        c->best = mon;
        c->bestWork = mi.rcWork;
        c->bestDist = d;
    }
    return TRUE;
}

static HMONITOR PickMonitor(POINT pt, bool primaryOnly, RECT* work) {
    MonPickCtx c{};
    c.pt = pt;
    c.primaryOnly = primaryOnly;
    EnumDisplayMonitors(NULL, NULL, PickMonitorProc, (LPARAM)&c);
    *work = c.best ? c.bestWork : FallbackWorkRect();
    return c.best;
}

RECT MonitorWorkRect(POINT pt) {
    RECT work{};
    PickMonitor(pt, false, &work);
    return work;
}

int MonitorScaleOf(POINT pt) {
    RECT work{};
    HMONITOR mon = PickMonitor(pt, false, &work);
    int dpi = DpiOfMonitorRaw(mon);
    if (dpi <= 0) {
        HDC dc = GetDC(NULL);
        dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(NULL, dc);
    }
    if (dpi <= 0) dpi = 96;
    int s = MulDiv(dpi, 100, 96);
    return s < 100 ? 100 : s;
}

RECT PrimaryMonitorWorkRect() {
    RECT work{};
    PickMonitor(POINT{INT_MIN, INT_MIN}, true, &work);
    return work;
}

void EnableDpiAwareness() {
    // Win10 1703+ 用 PER_MONITOR_AWARE_V2，失败再退回老 API
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI* SetCtxFn)(HANDLE);
        auto fn = (SetCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) return; // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    SetProcessDPIAware();
}

bool InitGdiPlus() {
    if (g_gdiplusToken) return true;
    GdiplusStartupInput input;
    return GdiplusStartup(&g_gdiplusToken, &input, NULL) == Ok;
}

void ShutdownGdiPlus() {
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

// ---------------------------------------------------------------- capture

bool CaptureVirtualScreen(Bitmap32& out) {
    RECT vs = GetVirtualScreenRect();
    const int w = vs.right - vs.left;
    const int h = vs.bottom - vs.top;
    if (w <= 0 || h <= 0) return false;

    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDC);
        ReleaseDC(NULL, screenDC);
        return false;
    }

    HGDIOBJ old = SelectObject(memDC, dib);
    BOOL ok = BitBlt(memDC, 0, 0, w, h, screenDC, vs.left, vs.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();

    if (ok) {
        out.w = w;
        out.h = h;
        out.px.resize((size_t)w * h);
        memcpy(out.px.data(), bits, (size_t)w * h * 4);
        // 强制不透明，避免个别应用给出 alpha=0 的黑图
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] |= 0xFF000000u;
    }

    SelectObject(memDC, old);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
    return ok != FALSE;
}

Bitmap32 CropBitmap(const Bitmap32& src, RECT rScreen) {
    RECT vs = GetVirtualScreenRect();
    int x0 = rScreen.left - vs.left;
    int y0 = rScreen.top - vs.top;
    int cw = rScreen.right - rScreen.left;
    int ch = rScreen.bottom - rScreen.top;
    if (x0 < 0) { cw += x0; x0 = 0; }
    if (y0 < 0) { ch += y0; y0 = 0; }
    if (x0 + cw > src.w) cw = src.w - x0;
    if (y0 + ch > src.h) ch = src.h - y0;

    Bitmap32 out;
    if (cw <= 0 || ch <= 0) return out;
    out.w = cw;
    out.h = ch;
    out.px.resize((size_t)cw * ch);
    for (int y = 0; y < ch; ++y)
        memcpy(out.row(y), src.row(y0 + y) + x0, (size_t)cw * 4);
    return out;
}

// ---------------------------------------------------------------- encode / clipboard

static int GetEncoderClsid(const WCHAR* mime, CLSID* out) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* codecs = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *out = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

static Bitmap* WrapBitmap(const Bitmap32& bm) {
    BYTE* bits = const_cast<BYTE*>((const BYTE*)bm.px.data());
    return new Bitmap(bm.w, bm.h, bm.w * 4, PixelFormat32bppARGB, bits);
}

bool EncodePngToMemory(const Bitmap32& bm, std::vector<BYTE>& out) {
    CLSID clsid;
    if (GetEncoderClsid(L"image/png", &clsid) < 0) return false;

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) return false;

    bool ok = false;
    Bitmap* bmp = WrapBitmap(bm);
    if (bmp) {
        if (bmp->Save(stream, &clsid, NULL) == Ok) {
            HGLOBAL h = NULL;
            if (GetHGlobalFromStream(stream, &h) == S_OK && h) {
                SIZE_T n = GlobalSize(h);
                BYTE* p = (BYTE*)GlobalLock(h);
                if (p) {
                    out.assign(p, p + n);
                    GlobalUnlock(h);
                    ok = true;
                }
            }
        }
        delete bmp;
    }
    stream->Release();
    return ok;
}

bool SavePngFile(const Bitmap32& bm, const std::wstring& path) {
    if (bm.empty()) return false;
    CLSID clsid;
    if (GetEncoderClsid(L"image/png", &clsid) < 0) return false;
    Bitmap* bmp = WrapBitmap(bm);
    if (!bmp) return false;
    Status st = bmp->Save(path.c_str(), &clsid, NULL);
    delete bmp;
    return st == Ok;
}

// CF_DIBV5：带 alpha 的剪贴板位图，现代应用（含 Chrome/QQ/微信/WPS）都认
static HGLOBAL MakeDibV5(const Bitmap32& bm) {
    const size_t header = sizeof(BITMAPV5HEADER);
    const size_t stride = (size_t)bm.w * 4;
    const size_t imageSize = stride * bm.h;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, header + imageSize);
    if (!h) return NULL;

    BYTE* p = (BYTE*)GlobalLock(h);
    memset(p, 0, header);
    BITMAPV5HEADER* v5 = (BITMAPV5HEADER*)p;
    v5->bV5Size = sizeof(BITMAPV5HEADER);
    v5->bV5Width = bm.w;
    v5->bV5Height = bm.h; // 正数 = bottom-up
    v5->bV5Planes = 1;
    v5->bV5BitCount = 32;
    v5->bV5Compression = BI_BITFIELDS;
    v5->bV5SizeImage = (DWORD)imageSize;
    v5->bV5RedMask = 0x00FF0000;
    v5->bV5GreenMask = 0x0000FF00;
    v5->bV5BlueMask = 0x000000FF;
    v5->bV5AlphaMask = 0xFF000000;
    v5->bV5CSType = 0x57696E20; // LCS_WINDOWS_COLOR_SPACE = 'Win '
    v5->bV5Intent = LCS_GM_IMAGES;

    BYTE* dst = p + header;
    for (int y = 0; y < bm.h; ++y) {
        const uint32_t* srcRow = bm.row(bm.h - 1 - y);
        memcpy(dst + (size_t)y * stride, srcRow, stride);
    }
    GlobalUnlock(h);
    return h;
}

bool CopyToClipboard(HWND owner, const Bitmap32& bm) {
    if (bm.empty()) return false;
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();

    bool any = false;

    // 1) 注册 "PNG" 格式：支持透明、跨应用最稳
    std::vector<BYTE> png;
    if (EncodePngToMemory(bm, png)) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (h) {
            void* p = GlobalLock(h);
            memcpy(p, png.data(), png.size());
            GlobalUnlock(h);
            UINT fmt = RegisterClipboardFormatW(L"PNG");
            if (fmt && SetClipboardData(fmt, h)) any = true;
            else GlobalFree(h);
        }
    }

    // 2) CF_DIBV5 兜底
    HGLOBAL dib = MakeDibV5(bm);
    if (dib) {
        if (SetClipboardData(CF_DIBV5, dib)) any = true;
        else GlobalFree(dib);
    }

    CloseClipboard();
    return any;
}

// ---------------------------------------------------------------- misc

void ZLog(const wchar_t* fmt, ...) {
    wchar_t msg[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1200] = {};
    swprintf(line, 1200, L"[%02d:%02d:%02d.%03d] %ls\r\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, msg);

    char utf8[2400] = {};
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 1) return;

    static std::mutex logMu;
    static bool rotatedChecked = false;
    std::lock_guard<std::mutex> lk(logMu);

    std::wstring logPath = DataDir() + L"zpix.log";
    if (!rotatedChecked) {
        rotatedChecked = true;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fad)) {
            ULONGLONG sz = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (sz > 2ull * 1024 * 1024) {
                std::wstring oldPath = logPath + L".old";
                MoveFileExW(logPath.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
        }
    }

    HANDLE h = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, utf8, (DWORD)(n - 1), &written, NULL);
    CloseHandle(h);
}

HDC CreateBitmapDC(const Bitmap32& bm, HBITMAP& outBmp, HGDIOBJ& oldObj) {
    outBmp = nullptr;
    oldObj = nullptr;
    if (bm.empty()) return nullptr;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.w;
    bi.bmiHeader.biHeight = -bm.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    outBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (outBmp && bits) memcpy(bits, bm.px.data(), bm.px.size() * 4);
    oldObj = SelectObject(dc, outBmp);
    ReleaseDC(NULL, screen);
    return dc;
}

std::wstring ExeDirPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    return path;
}

static bool DirWritable(const std::wstring& dir) {
    std::wstring probe = dir + L".zpix_probe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

static bool IsInstalledCopy(const std::wstring& exeDir) {
    // 1. 存在卸载程序即视为安装版（无论安装到 Program Files 还是自定义目录）
    std::wstring uninst = exeDir + L"uninstall.exe";
    if (GetFileAttributesW(uninst.c_str()) != INVALID_FILE_ATTRIBUTES) return true;

    // 2. 位于 Program Files / Program Files (x86) 下的也统一视为安装版
    auto underKnown = [&](REFKNOWNFOLDERID fid) -> bool {
        PWSTR p = nullptr;
        if (FAILED(SHGetKnownFolderPath(fid, 0, NULL, &p)) || !p) return false;
        std::wstring base = p;
        CoTaskMemFree(p);
        if (!base.empty() && base.back() != L'\\') base += L'\\';
        return exeDir.size() >= base.size() &&
               _wcsnicmp(exeDir.c_str(), base.c_str(), base.size()) == 0;
    };
    return underKnown(FOLDERID_ProgramFiles) || underKnown(FOLDERID_ProgramFilesX86);
}

// 数据目录：安装版始终统一使用 %LOCALAPPDATA%\Zpix（避免管理员权限启动与普通启动
// 分别读写安装目录和 AppData 导致配置分叉）；绿色便携版旁可写就放 exe 同目录。
std::wstring DataDir() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    std::wstring exe = ExeDirPath();
    // 测试/高级用法：ZPIX_DATADIR 可直接指定数据目录
    wchar_t envDir[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ZPIX_DATADIR", envDir, MAX_PATH) > 0 && envDir[0]) {
        std::wstring dir = envDir;
        if (dir.back() != L'\\') dir += L'\\';
        CreateDirectoryW(dir.c_str(), NULL);
        cached = dir;
        return cached;
    }
    if (!IsInstalledCopy(exe) && DirWritable(exe)) {
        cached = exe;
        return cached;
    }
    std::wstring base;
    PWSTR folder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &folder)) && folder) {
        base = folder;
        CoTaskMemFree(folder);
    } else {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        base = tmp;
    }
    std::wstring dir = base + L"\\Zpix";
    for (size_t i = dir.find(L'\\', 3); i != std::wstring::npos; i = dir.find(L'\\', i + 1))
        CreateDirectoryW(dir.substr(0, i).c_str(), NULL);
    CreateDirectoryW(dir.c_str(), NULL);
    cached = dir + L"\\";
    return cached;
}

// 读取脚本头部里的版本标记（ocr.ps1 里的 "# ZPIX_OCR_VERSION=3"）
static int ScriptVersionOf(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return -1;
    char buf[512] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    std::string s(buf, n);
    const char* key = "ZPIX_OCR_VERSION=";
    size_t p = s.find(key);
    if (p == std::string::npos) return 0;
    return atoi(s.c_str() + p + strlen(key));
}

void EnsureDataFiles() {
    std::wstring data = DataDir();
    std::wstring exe = ExeDirPath();
    if (data == exe) return;

    // config.ini 是用户数据：只补缺，绝不覆盖
    {
        std::wstring dst = data + L"config.ini";
        std::wstring src = exe + L"config.ini";
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(src.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
                ZLog(L"[env] migrate %ls -> %ls", src.c_str(), dst.c_str());
            else
                ZLog(L"[env] migrate failed %ls err=%lu", src.c_str(), GetLastError());
        }
    }

    // ocr.ps1 是程序的一部分：版本号变了就覆盖（否则升级后还在用旧脚本，
    // 识别效果跟不上安装包里那份）
    {
        std::wstring dst = data + L"ocr.ps1";
        std::wstring src = exe + L"ocr.ps1";
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        bool need = (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES);
        if (!need) {
            int vs = ScriptVersionOf(src);
            int vd = ScriptVersionOf(dst);
            if (vs != vd) {
                need = true;
                ZLog(L"[env] ocr.ps1 version %d -> %d", vd, vs);
            }
        }
        if (!need) return;
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
            ZLog(L"[env] migrate %ls -> %ls", src.c_str(), dst.c_str());
        else
            ZLog(L"[env] migrate failed %ls err=%lu", src.c_str(), GetLastError());
    }
}

std::wstring NormalizeDirPath(const std::wstring& dir) {
    std::wstring s = dir;
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'"'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'"'))
        s.pop_back();
    for (wchar_t& c : s) {
        if (c == L'/') c = L'\\';
    }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

bool EnsureDirectoryRecursive(const std::wstring& dir) {
    std::wstring clean = NormalizeDirPath(dir);
    if (clean.empty()) return false;
    DWORD attr = GetFileAttributesW(clean.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    size_t start = 0;
    if (clean.size() >= 2 && clean[1] == L':') start = 3;          // C:\...
    else if (clean.size() >= 2 && clean[0] == L'\\' && clean[1] == L'\\') {
        size_t s1 = clean.find(L'\\', 2);
        size_t s2 = (s1 == std::wstring::npos) ? std::wstring::npos : clean.find(L'\\', s1 + 1);
        start = (s2 == std::wstring::npos) ? clean.size() : s2 + 1;
    }

    for (size_t i = clean.find(L'\\', start); i != std::wstring::npos; i = clean.find(L'\\', i + 1)) {
        std::wstring sub = clean.substr(0, i);
        if (!sub.empty()) CreateDirectoryW(sub.c_str(), NULL);
    }
    CreateDirectoryW(clean.c_str(), NULL);
    attr = GetFileAttributesW(clean.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring AppDataSaveDir() {
    PWSTR pics = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, NULL, &pics)) && pics) {
        dir = pics;
        CoTaskMemFree(pics);
    } else {
        wchar_t buf[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, buf);
        dir = buf;
    }
    dir = NormalizeDirPath(dir);
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"Zpix";
    EnsureDirectoryRecursive(dir);
    return dir;
}

std::wstring EffectiveSaveRootDir(const std::wstring& configuredDir) {
    std::wstring norm = NormalizeDirPath(configuredDir);
    if (!norm.empty()) return norm;
    return AppDataSaveDir();
}

std::wstring TodayDateFolderName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring TimestampFileName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"Zpix_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring BuildDailyShotPath(const std::wstring& configuredDir, std::wstring* outDateDir) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t dateBuf[32];
    swprintf(dateBuf, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    std::wstring root = EffectiveSaveRootDir(configuredDir);
    std::wstring dateDir = JoinPath(root, dateBuf);
    if (!EnsureDirectoryRecursive(dateDir)) {
        // 自定义目录不可达（如移动盘拔出、非法盘符）时自动退回默认保存目录，避免丢图
        ZLog(L"[save] custom dir unavailable: %ls -> fallback default", dateDir.c_str());
        root = AppDataSaveDir();
        dateDir = JoinPath(root, dateBuf);
        EnsureDirectoryRecursive(dateDir);
    }
    if (outDateDir) *outDateDir = dateDir;

    wchar_t baseName[64];
    swprintf(baseName, 64, L"Zpix_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    std::wstring candidate = JoinPath(dateDir, std::wstring(baseName) + L".png");
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;

    // 同一秒连续截图：补毫秒 / 递增序号，绝不覆盖旧图
    wchar_t msName[80];
    swprintf(msName, 80, L"%ls_%03d.png", baseName, st.wMilliseconds);
    candidate = JoinPath(dateDir, msName);
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;

    for (int i = 1; i <= 999; ++i) {
        wchar_t seqName[96];
        swprintf(seqName, 96, L"%ls_%03d_%02d.png", baseName, st.wMilliseconds, i);
        candidate = JoinPath(dateDir, seqName);
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return candidate;
}

std::wstring PreviewDailyShotPath(const std::wstring& configuredDir) {
    std::wstring root = NormalizeDirPath(configuredDir);
    if (root.empty()) {
        PWSTR pics = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, NULL, &pics)) && pics) {
            root = JoinPath(NormalizeDirPath(pics), L"Zpix");
            CoTaskMemFree(pics);
        } else {
            root = L"Pictures\\Zpix";
        }
    }
    return JoinPath(JoinPath(root, TodayDateFolderName()), TimestampFileName());
}

void OpenFolderInExplorer(const std::wstring& dir) {
    if (dir.empty()) return;
    EnsureDirectoryRecursive(dir);
    ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void OpenFolderAndSelectFile(const std::wstring& filePath, const std::wstring& fallbackDir) {
    if (!filePath.empty() && GetFileAttributesW(filePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring arg = L"/select,\"" + filePath + L"\"";
        HINSTANCE r = ShellExecuteW(NULL, L"open", L"explorer.exe", arg.c_str(), NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r > 32) return;
    }
    if (!fallbackDir.empty()) {
        OpenFolderInExplorer(fallbackDir);
    }
}

bool PickFolderDialog(HWND owner, const std::wstring& initialDir, std::wstring& outDir) {
    HRESULT hrInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needUninit = SUCCEEDED(hrInit);

    bool picked = false;
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog,
                                  (void**)&dlg);
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        dlg->SetTitle(L"选择截图保存主目录");
        std::wstring initNorm = NormalizeDirPath(initialDir);
        if (!initNorm.empty() &&
            GetFileAttributesW(initNorm.c_str()) != INVALID_FILE_ATTRIBUTES) {
            IShellItem* initItem = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initNorm.c_str(), NULL, IID_IShellItem,
                                                      (void**)&initItem)) &&
                initItem) {
                dlg->SetFolder(initItem);
                initItem->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    outDir = NormalizeDirPath(path);
                    CoTaskMemFree(path);
                    picked = !outDir.empty();
                }
                item->Release();
            }
        }
        dlg->Release();
    } else {
        // 老系统或 COM 不可用时回退 SHBrowseForFolderW
        BROWSEINFOW bi{};
        bi.hwndOwner = owner;
        bi.lpszTitle = L"选择截图保存主目录";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            wchar_t path[MAX_PATH] = {};
            if (SHGetPathFromIDListW(pidl, path) && path[0]) {
                outDir = NormalizeDirPath(path);
                picked = !outDir.empty();
            }
            CoTaskMemFree(pidl);
        }
    }

    if (needUninit) CoUninitialize();
    return picked;
}

HFONT MakeUiFont(int px, bool bold) {
    return CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void DrawTextShadow(HDC dc, const std::wstring& text, RECT rc, HFONT font,
                    COLORREF color, UINT fmt) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    RECT shadow = rc;
    shadow.left += 1;
    shadow.top += 1;
    SetTextColor(dc, RGB(0, 0, 0));
    DrawTextW(dc, text.c_str(), (int)text.size(), &shadow, fmt);

    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), (int)text.size(), &rc, fmt);
    SelectObject(dc, oldFont);
}