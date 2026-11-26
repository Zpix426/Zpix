#include "util.h"
#include "overlay.h"
#include "pin.h"
#include "actions.h"
#include "config.h"
#include "img_translate.h"
#include "select_translate.h"
#include "settings.h"

#include <shellapi.h>
#include <sddl.h>
#include <thread>
#include <vector>
#include <string>

HWND g_msgWnd = nullptr;
static HICON g_icon = nullptr;
static HICON g_trayIcon = nullptr;
static std::wstring g_hotkeyText = L"F1";

static const UINT WM_TRAY_CALLBACK = WM_APP + 1;
static const UINT WM_ZPIX_SHOW_SETTINGS = WM_APP + 21;  // 第二次启动时把设置窗口叫到前台
static UINT g_wmTaskbarCreated = 0;                     // 资源管理器重启后重新注册托盘图标
static const UINT TRAY_ID = 1;

static bool g_trayOk = false;
static int g_trayRetry = 0;

static const int MENU_SHOT = 1;
static const int MENU_SETTINGS = 2;
static const int MENU_OPEN_DIR = 3;
static const int MENU_EXIT = 4;
static const int MENU_SELECT_TR = 5;

// 应用图标：优先用打包进 exe 的 logo（src/app.ico → 资源 id 1）
HICON MakeAppIcon() {
    const int sizes[] = {64, 48, 32, 256};
    for (int sz : sizes) {
        HICON h = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz,
                                    LR_DEFAULTCOLOR);
        if (h) return h;
    }
    return (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, 32, 32, LR_SHARED);
}

// 托盘专用放大高清图标（src/tray.ico → 资源 id 2，按当前系统托盘精确像素尺寸加载避免系统缩放变糊）
HICON MakeTrayIcon() {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx < 16) cx = 16;
    if (cy < 16) cy = 16;
    HICON h = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(2), IMAGE_ICON, cx, cy,
                                LR_DEFAULTCOLOR);
    if (h) return h;
    return MakeAppIcon();
}

// 当前进程的完整性级别（0x1000=低 0x2000=中 0x3000=高）
static DWORD SelfIntegrity() {
    DWORD rid = 0;
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD need = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &need);
        TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)LocalAlloc(LMEM_FIXED, need);
        if (tml && GetTokenInformation(tok, TokenIntegrityLevel, tml, need, &need)) {
            PSID sid = tml->Label.Sid;
            rid = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
        }
        if (tml) LocalFree(tml);
        CloseHandle(tok);
    }
    return rid;
}

// 全局截图热键（动作键 F2~F5 只在覆盖层里生效，不是全局）
void ApplyHotkeys() {
    if (!g_msgWnd) return;
    UnregisterHotKey(g_msgWnd, 1);
    UINT mods = 0, vk = VK_F1;
    if (!ParseHotkey(g_cfg.screenshot, &mods, &vk)) {
        mods = 0;
        vk = VK_F1;
    }
    BOOL ok = RegisterHotKey(g_msgWnd, 1, mods | MOD_NOREPEAT, vk);
    if (!ok) {
        UnregisterHotKey(g_msgWnd, 1);
        mods = MOD_CONTROL | MOD_ALT;
        vk = 'A';
        ok = RegisterHotKey(g_msgWnd, 1, mods | MOD_NOREPEAT, vk);
        ZLog(L"hotkey fallback -> %d", (int)ok);
    }
    g_hotkeyText = FormatHotkey(mods, vk);
    ZLog(L"hotkey registered=%ls", g_hotkeyText.c_str());
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    std::wstring shotItem = L"截图\t" + g_hotkeyText;
    AppendMenuW(menu, MF_STRING, MENU_SHOT, shotItem.c_str());
    AppendMenuW(menu, MF_STRING | (g_cfg.enableSelectTranslate ? MF_CHECKED : MF_UNCHECKED),
                MENU_SELECT_TR, L"全局划词翻译");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_SETTINGS, L"设置…");
    AppendMenuW(menu, MF_STRING, MENU_OPEN_DIR, L"打开保存目录");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, MENU_EXIT, L"退出 Zpix");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void OpenSaveDir() {
    OpenFolderInExplorer(EffectiveSaveRootDir(g_cfg.saveDir));
}

static bool AddTrayIcon(HWND hwnd) {
    if (!g_trayIcon) g_trayIcon = MakeTrayIcon();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = g_trayIcon ? g_trayIcon : g_icon;
    wcscpy(nid.szTip, L"Zpix 截图 · 右键打开设置");
    BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
    ZLog(L"tray add ok=%d", (int)ok);
    NOTIFYICONDATAW ver{};
    ver.cbSize = sizeof(ver);
    ver.hWnd = hwnd;
    ver.uID = TRAY_ID;
    ver.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &ver);
    g_trayOk = (ok != FALSE);
    return g_trayOk;
}

// 托盘气泡（启动/重复启动时给用户明确反馈）
static void TrayBalloon(HWND hwnd, const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy(nid.szInfoTitle, title, ARRAYSIZE(nid.szInfoTitle) - 1);
    wcsncpy(nid.szInfo, text, ARRAYSIZE(nid.szInfo) - 1);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
LRESULT CALLBACK MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {    if (g_wmTaskbarCreated && msg == g_wmTaskbarCreated) {
        ZLog(L"[tray] TaskbarCreated -> re-add");
        AddTrayIcon(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        if (wp == 1) {
            ZLog(L"WM_HOTKEY id=1");
            StartScreenshot();
        }
        return 0;
    case WM_ZPIX_TRANSLATE_DONE: {
        TranslatePayload* r = (TranslatePayload*)lp;
        if (r) {
            if (r->target && IsWindow(r->target)) {
                SetResultPayload(r->target, r->original, r->results, r->reqId);
            } else if (r->reqId == 0) {
                HWND w = ShowResultWindow(r->original);
                SetResultPayload(w, r->original, r->results, 0);
            }
            delete r;
        }
        return 0;
    }
    case WM_ZPIX_OCR_DONE:
        HandleOcrDone((OcrPayload*)lp);
        return 0;
    case WM_ZPIX_IMGSEARCH_DONE:
        HandleImgSearchDone((ImgSearchDonePayload*)lp);
        return 0;
    case WM_ZPIX_SELECT_TRIGGER:
    case WM_ZPIX_SELECT_READY:
    case WM_ZPIX_SELECT_HIDE:
        HandleSelectMessage(msg, wp, lp);
        return 0;
    case WM_ZPIX_SHOW_SETTINGS: {
        // 用户又双击了一次 exe 或安装完成拉起：把设置窗口带到前台（lp!=0 时切换到指定 Tab）
        int targetTab = (lp != 0) ? (int)wp : -1;
        ShowSettingsWindow(hwnd, targetTab);
        if (lp == 0 && g_trayOk) {
            TrayBalloon(hwnd, L"Zpix 已在运行", L"已为你打开设置窗口；右键托盘图标可随时打开。");
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 3) {
            if (AddTrayIcon(hwnd)) {
                KillTimer(hwnd, 3);
                ZLog(L"[tray] retry ok after %d", g_trayRetry);
                ShowToast(L"托盘图标已就绪");
            } else if (++g_trayRetry >= 5) {
                KillTimer(hwnd, 3);
                ZLog(L"[tray] retry gave up");
                ShowToast(L"托盘图标被拦下：请从桌面快捷方式正常启动一次");
                ShowSettingsWindow(hwnd);
            }
            return 0;
        }
        break;
    case WM_TRAY_CALLBACK:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
            return 0;
        }
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            StartScreenshot();
            return 0;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case MENU_SHOT:
            StartScreenshot();
            return 0;
        case MENU_SELECT_TR:
            g_cfg.enableSelectTranslate = !g_cfg.enableSelectTranslate;
            if (!g_cfg.enableSelectTranslate) HideSelectBar();
            SaveSettings();
            ShowToast(g_cfg.enableSelectTranslate ? L"全局划词翻译已开启" : L"全局划词翻译已关闭");
            return 0;
        case MENU_SETTINGS:
            ShowSettingsWindow(hwnd);
            return 0;
        case MENU_OPEN_DIR:
            OpenSaveDir();
            return 0;
        case MENU_EXIT: {
            StopSelectMonitor();
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = TRAY_ID;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            PostQuitMessage(0);
            return 0;
        }
        }
        return 0;
    case WM_DESTROY:
        StopSelectMonitor();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    ZLog(L"WinMain enter");
    EnsureDataFiles();

    if (wcsstr(GetCommandLineW(), L"--datadir")) {
        wchar_t tdir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tdir);
        std::wstring d = DataDir();
        std::wstring e = ExeDirPath();
        std::wstring info = L"exe=" + e + L"\r\ndata=" + d + L"\r\ntemp=" + tdir +
                            L"\r\nwritable=" + ((d == e) ? L"1" : L"0") + L"\r\n";
        std::wstring probePath = std::wstring(tdir) + L"zpix_probe.txt";
        WriteTextFileUtf8(probePath, info);
        return 0;
    }
    if (wcsstr(GetCommandLineW(), L"--selftest")) {
        InitGdiPlus();
        LoadSettings();
        Bitmap32 bm;
        bool cap = CaptureVirtualScreen(bm);
        ZLog(L"[selftest] capture=%d %dx%d", (int)cap, bm.w, bm.h);
        RECT vs = GetVirtualScreenRect();
        RECT r{vs.left + 100, vs.top + 100, vs.left + 700, vs.top + 500};
        Bitmap32 crop = CropBitmap(bm, r);
        ZLog(L"[selftest] crop=%dx%d", crop.w, crop.h);
        std::wstring out = BuildDailyShotPath(g_cfg.saveDir);
        ZLog(L"[selftest] save=%d path=%ls", (int)SavePngFile(crop, out), out.c_str());
        CreatePinFromBitmap(crop, POINT{400, 400});
        ZLog(L"[selftest] done");
        Sleep(150);
        ShutdownGdiPlus();
        return 0;
    }

    // ---- 开发/自测入口 ----
    {
        LPWSTR cmd = GetCommandLineW();
        auto argAfter = [&](const wchar_t* flag) -> std::wstring {
            const wchar_t* p = wcsstr(cmd, flag);
            if (!p) return L"";
            p += wcslen(flag);
            while (*p == L' ' || *p == L'"') ++p;
            std::wstring s;
            while (*p && *p != L'"') { s += *p; ++p; }
            return s;
        };
        std::wstring imgTrPath = argAfter(L"--img-translate");
        if (!imgTrPath.empty()) {
            InitGdiPlus();
            LoadSettings();
            std::wstring outPng = DataDir() + L"img_tr_out.png";
            img_translate::Result res;
            bool ok = img_translate::TranslateImageFile(imgTrPath, outPng, res);
            std::wstring report = ok ? (L"OK blocks=" + std::to_wstring(res.blockCount) +
                                        L" ocrMs=" + std::to_wstring(res.ocrMs) +
                                        L" trMs=" + std::to_wstring(res.trMs) +
                                        L" prov=" + res.provider +
                                        L" lang=" + res.targetLang +
                                        L" out=" + outPng)
                                     : (L"ERR: " + res.error);
            WriteTextFileUtf8(DataDir() + L"cli_result.txt", report);
            ZLog(L"[cli] img-translate %ls", report.c_str());
            ShutdownGdiPlus();
            return ok ? 0 : 1;
        }
        std::wstring ocrPath = argAfter(L"--ocr");
        if (!ocrPath.empty()) {
            std::wstring text = OcrImageFile(ocrPath);
            std::wstring out = ExeDirOf().empty() ? std::wstring(L"cli_result.txt")
                                                  : DataDir() + L"cli_result.txt";
            WriteTextFileUtf8(out, text);
            ZLog(L"[cli] ocr chars=%d", (int)text.size());
            return 0;
        }
        std::wstring trText = argAfter(L"--translate ");
        if (!trText.empty()) {
            LoadSettings();
            std::vector<TrResult> rs = TranslateAll(trText);
            std::wstring joined;
            for (const auto& r : rs) {
                joined += L"[" + r.provider + L" / " + std::to_wstring(r.ms) + L"ms]\r\n";
                joined += (r.text.empty() ? (L"ERR: " + r.error) : r.text);
                joined += L"\r\n\r\n";
            }
            std::wstring out = ExeDirOf().empty() ? std::wstring(L"cli_result.txt")
                                                  : DataDir() + L"cli_result.txt";
            WriteTextFileUtf8(out, joined);
            ZLog(L"[cli] translate providers=%d chars=%d", (int)rs.size(), (int)joined.size());
            return 0;
        }
    }
    EnableDpiAwareness();
    if (!InitGdiPlus()) ZLog(L"GdiplusStartup failed");
    {
        DWORD il = SelfIntegrity();
        ZLog(L"[env] integrity=0x%lX", il);
        if (il && il < 0x2000)
            ZLog(L"[env] 进程被降权：托盘图标会被系统拦下（正常双击启动不会这样）");
    }

    const bool firstRun = (GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES);
    ZLog(L"first run=%d", (int)firstRun);
    LoadSettings();

    // Zpix.exe --exit：请求已运行实例退出（托盘不可用时的退出入口）
    if (wcsstr(GetCommandLineW(), L"--exit")) {
        HWND running = FindWindowW(L"ZpixMsgWindow", NULL);
        if (running) {
            PostMessageW(running, WM_CLOSE, 0, 0);
            ZLog(L"--exit forwarded");
        }
        return 0;
    }

    const bool afterInstall = (wcsstr(GetCommandLineW(), L"--after-install") != nullptr);
    HANDLE mutex = CreateMutexW(NULL, FALSE, L"Zpix_SingleInstance_Mutex");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // 已经有实例在跑：不再静默退出，直接把「设置」窗口叫到前台（不然用户以为双击没反应）
        HWND running = FindWindowW(L"ZpixMsgWindow", NULL);
        if (running) {
            PostMessageW(running, WM_ZPIX_SHOW_SETTINGS,
                         afterInstall ? (WPARAM)SETTINGS_TAB_TRANSLATE : 0,
                         afterInstall ? 1 : 0);
            ZLog(L"another instance is running -> forward show-settings");
        } else {
            ZLog(L"another instance is running, but window not found");
        }
        return 0;
    }

    g_icon = MakeAppIcon();
    g_trayIcon = MakeTrayIcon();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MsgProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ZpixMsgWindow";
    RegisterClassExW(&wc);

    // 隐藏的顶层消息窗口（不用 HWND_MESSAGE：便于自测脚本按类名定位并投递命令）
    g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"ZpixMsgWindow", L"Zpix", WS_POPUP, 0, 0, 0, 0,
                               NULL, NULL, hInst, NULL);
    ZLog(L"=== Zpix boot ===");

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ApplyHotkeys();
    StartSelectMonitor(g_msgWnd);
    bool trayOk = AddTrayIcon(g_msgWnd);
    ZLog(L"tray added ok=%d", (int)trayOk);
    if (!trayOk) {
        ZLog(L"[tray] add failed -> retry timer");
        SetTimer(g_msgWnd, 3, 2500, NULL);
    }

    // 启动反馈：气泡 + 轻提示（否则「双击没反应」）
    if (trayOk) {
        std::wstring tip = g_hotkeyText + L" 截图；右键托盘图标打开设置。";
        TrayBalloon(g_msgWnd, L"Zpix 已启动", tip.c_str());
    }
    if (firstRun || afterInstall) {
        ZLog(L"open settings on translate tab (firstRun=%d afterInstall=%d)", (int)firstRun,
             (int)afterInstall);
        bool zhipuMissingKey = true;
        for (const auto& p : g_cfg.providers) {
            if (p.kind == TrKind::Zhipu && !p.key.empty()) {
                zhipuMissingKey = false;
                break;
            }
        }
        Sleep(200);
        ShowSettingsWindow(g_msgWnd, SETTINGS_TAB_TRANSLATE);
        if (zhipuMissingKey) {
            ShowToast(L"当前智谱AI没有 Key（默认已关闭），已默认开启 BliBli 与 Google 翻译");
        } else {
            ShowToast(L"安装完成：已为你打开翻译服务设置");
        }
    } else if (wcsstr(GetCommandLineW(), L"--settings")) {
        Sleep(200);
        ShowSettingsWindow(g_msgWnd);
    } else {
        ShowToast(L"Zpix 已在后台运行");
    }

    // 自测入口：直接打开翻译结果窗口，跑一次多厂家翻译
    {
        const wchar_t* flag = L"--translate-ui ";
        const wchar_t* p = wcsstr(GetCommandLineW(), flag);
        if (p) {
            p += wcslen(flag);
            std::wstring text;
            while (*p && *p != L'"') {
                text += *p;
                ++p;
            }
            if (!text.empty()) {
                Sleep(200);
                HWND w = ShowResultWindow(text);
                std::thread([text, w]() {
                    std::vector<TrResult> res = TranslateAll(text);
                    TranslatePayload* payload = new TranslatePayload();
                    payload->original = text;
                    payload->results = res;
                    payload->target = w;
                    PostMessageW(g_msgWnd, WM_ZPIX_TRANSLATE_DONE, 0, (LPARAM)payload);
                }).detach();
            }
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    ShutdownGdiPlus();
    return 0;
}