// Zpix 现代单页安装 / 卸载程序（payload 内嵌为 RCDATA）
// 需要管理员权限（写 Program Files + HKLM 卸载项），清单见 src/setup.manifest。

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <knownfolders.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#define RES_ZPIX_EXE 101
#define RES_OCR_PS1 102
#define RES_ORT_DLL 201
#define RES_DET_ONNX 202
#define RES_REC_ONNX 203
#define RES_KEYS_TXT 204

static const wchar_t* kAppName = L"Zpix 截图";
static const wchar_t* kAppVersion = L"0.4.0";
static const wchar_t* kAppTagline = L"截图贴图 · 本地 OCR · 多引擎翻译 · 全局划词";
static const wchar_t* kRegUninstall =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Zpix";
static const wchar_t* kRegRun =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kMutexName = L"Zpix_Setup_Mutex";

static const UINT WM_SETUP_PROGRESS = WM_APP + 10;
static const UINT WM_SETUP_DONE = WM_APP + 11;
static const UINT_PTR kAnimTimer = 1;

static const COLORREF kBg = RGB(255, 255, 255);
static const COLORREF kPanel = RGB(247, 248, 250);
static const COLORREF kAccent = RGB(240, 144, 21);
static const COLORREF kAccentHover = RGB(224, 129, 11);
static const COLORREF kAccentPress = RGB(204, 115, 8);
static const COLORREF kAccentSoft = RGB(255, 244, 229);
static const COLORREF kText = RGB(31, 35, 41);
static const COLORREF kSub = RGB(100, 106, 115);
static const COLORREF kMuted = RGB(143, 149, 158);
static const COLORREF kBorder = RGB(224, 228, 235);
static const COLORREF kGreen = RGB(22, 163, 74);
static const COLORREF kGreenSoft = RGB(220, 252, 231);
static const COLORREF kRed = RGB(220, 38, 38);
static const COLORREF kRedSoft = RGB(254, 226, 226);

enum View { V_INSTALL = 0, V_LICENSE, V_PROGRESS, V_DONE, V_UNINSTALL };

enum HitId {
    H_NONE = 0,
    H_MIN,
    H_CLOSE,
    H_PRIMARY,
    H_SECONDARY,
    H_AGREE,
    H_LICENSE,
    H_EXPAND,
    H_BROWSE,
    H_CHK_DESK,
    H_CHK_MENU,
    H_CHK_AUTO,
    H_CHK_RUN,
    H_LOG,
};

static const wchar_t* kLicenseText =
    L"MIT License\r\n\r\n"
    L"Copyright (c) 2026 Zpix\r\n\r\n"
    L"Permission is hereby granted, free of charge, to any person obtaining a copy\r\n"
    L"of this software and associated documentation files (the \"Software\"), to deal\r\n"
    L"in the Software without restriction, including without limitation the rights\r\n"
    L"to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\r\n"
    L"copies of the Software, and to permit persons to whom the Software is\r\n"
    L"furnished to do so, subject to the following conditions:\r\n\r\n"
    L"The above copyright notice and this permission notice shall be included in all\r\n"
    L"copies or substantial portions of the Software.\r\n\r\n"
    L"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\r\n"
    L"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\r\n"
    L"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\r\n"
    L"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\r\n"
    L"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\r\n"
    L"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\r\n"
    L"SOFTWARE.\r\n";

struct AppState {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HWND hEditDir = nullptr;
    HWND hEditLic = nullptr;
    HICON hIconBig = nullptr;
    HICON hIconSm = nullptr;
    HBRUSH brWhite = nullptr;
    ULONG_PTR gdiToken = 0;

    int dpi = 96;
    int W = 520;
    int H = 372;

    View view = V_INSTALL;
    bool uninstallMode = false;
    bool silent = false;
    bool isUpgrade = false;
    bool expanded = false;

    bool agreed = true;
    bool optDesktop = true;
    bool optMenu = true;
    bool optAutoStart = true;
    bool optLaunch = true;

    std::wstring dir;
    std::wstring statusText;
    std::wstring failReason;
    bool success = false;
    bool busy = false;
    bool launched = false;

    int targetPct = 0;
    float dispPct = 0.0f;

    int hover = H_NONE;
    int pressed = H_NONE;
    bool tracking = false;

    HFONT fTitle = nullptr, fH1 = nullptr, fH2 = nullptr, fBody = nullptr, fSmall = nullptr,
          fBtn = nullptr, fPct = nullptr;

    RECT rcMin{}, rcClose{};
    RECT rcPrimary{}, rcSecondary{};
    RECT rcAgree{}, rcLicLink{}, rcExpand{};
    RECT rcOptCard{}, rcDirBox{}, rcBrowse{};
    RECT rcChkDesk{}, rcChkMenu{}, rcChkAuto{}, rcChkRun{};
    RECT rcLicBox{}, rcLogBtn{};
};

static AppState g;

static int S(int v) { return MulDiv(v, g.dpi, 96); }

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring s = buf;
    size_t p = s.find_last_of(L'\\');
    return p == std::wstring::npos ? std::wstring() : s.substr(0, p + 1);
}

static std::wstring LogFilePath() {
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"zpix_setup.log";
}

static void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 2048, fmt, ap);
    va_end(ap);
    static std::wstring logPath;
    if (logPath.empty()) logPath = LogFilePath();
    FILE* f = _wfopen(logPath.c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d] %ls\n", st.wHour, st.wMinute, st.wSecond, buf);
    fclose(f);
}

static std::wstring KnownDir(REFKNOWNFOLDERID id) {
    wchar_t* p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

static std::wstring DefaultInstallDir() {
    std::wstring pf = KnownDir(FOLDERID_ProgramFiles);
    return pf.empty() ? L"C:\\Program Files\\Zpix" : (pf + L"\\Zpix");
}

static std::wstring QueryInstalledDir() {
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, kRegUninstall, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            wchar_t buf[MAX_PATH] = {};
            DWORD sz = sizeof(buf), type = 0;
            std::wstring dir;
            if (RegQueryValueExW(key, L"InstallLocation", NULL, &type, (BYTE*)buf, &sz) ==
                ERROR_SUCCESS)
                dir = buf;
            RegCloseKey(key);
            if (!dir.empty()) return dir;
        }
    }
    return L"";
}

static bool HasAutoStart() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRun, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, sz = 0;
    bool ok = (RegQueryValueExW(key, L"Zpix", NULL, &type, NULL, &sz) == ERROR_SUCCESS && sz > 0);
    RegCloseKey(key);
    return ok;
}

static void SetAutoStart(bool enable, const std::wstring& exe) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegRun, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) !=
        ERROR_SUCCESS)
        return;
    if (enable) {
        std::wstring val = L"\"" + exe + L"\"";
        RegSetValueExW(key, L"Zpix", 0, REG_SZ, (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"Zpix");
    }
    RegCloseKey(key);
    Log(L"autostart=%d", (int)enable);
}

static std::wstring UninstallTargetDir() {
    std::wstring exeDir = ExeDir();
    while (!exeDir.empty() && exeDir.back() == L'\\') exeDir.pop_back();
    if (!exeDir.empty() &&
        GetFileAttributesW((exeDir + L"\\Zpix.exe").c_str()) != INVALID_FILE_ATTRIBUTES)
        return exeDir;
    std::wstring reg = QueryInstalledDir();
    return reg.empty() ? DefaultInstallDir() : reg;
}

static bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    std::wstring cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        bool sep = (path[i] == L'\\' || path[i] == L'/');
        if (!sep && i + 1 != path.size()) continue;
        if (cur.size() == 3 && cur[1] == L':') continue;
        CreateDirectoryW(cur.c_str(), NULL);
    }
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static bool WriteResToFile(int id, const std::wstring& dst, std::wstring* err) {
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) {
        if (err) *err = L"安装包资源缺失（#" + std::to_wstring(id) + L"），请重新下载安装包。";
        return false;
    }
    DWORD size = SizeofResource(NULL, res);
    HGLOBAL mem = LoadResource(NULL, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data || !size) {
        if (err) *err = L"安装包资源损坏，请重新下载。";
        return false;
    }
    HANDLE f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (err) {
            *err = L"无法写入 " + dst + L"（错误码 " + std::to_wstring(e) + L"）";
            if (e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION)
                *err += L"，请关闭正在运行的 Zpix 后重试。";
        }
        Log(L"create failed %ls err=%lu", dst.c_str(), e);
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, NULL);
    CloseHandle(f);
    if (!ok || written != size) {
        if (err) *err = L"写入不完整：" + dst;
        return false;
    }
    Log(L"write %ls %lu bytes", dst.c_str(), written);
    return true;
}

static bool MakeLink(const std::wstring& lnkPath, const std::wstring& target,
                     const std::wstring& args, const std::wstring& workDir,
                     const std::wstring& desc) {
    EnsureDir(lnkPath.substr(0, lnkPath.find_last_of(L'\\')));
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                (void**)&sl)))
        return false;
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetDescription(desc.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
        pf->Release();
    }
    sl->Release();
    return ok;
}

static void StopRunningApp(const std::wstring& dir) {
    HWND running = FindWindowW(L"ZpixMsgWindow", NULL);
    if (running) PostMessageW(running, WM_CLOSE, 0, 0);

    std::wstring exe = dir + L"\\Zpix.exe";
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring cmd = L"\"" + exe + L"\" --exit";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir.c_str(),
                           &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
    for (int i = 0; i < 12; ++i) {
        if (!FindWindowW(L"ZpixMsgWindow", NULL)) break;
        Sleep(100);
    }
    Sleep(150);
}

static bool SetUninstallEntry(const std::wstring& dir, const std::wstring& uninst) {
    std::wstring exe = dir + L"\\Zpix.exe";
    auto write = [&](HKEY root) -> LSTATUS {
        HKEY key = nullptr;
        LSTATUS rc = RegCreateKeyExW(root, kRegUninstall, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
        if (rc != ERROR_SUCCESS) return rc;
        auto sz = [&](const wchar_t* name, const std::wstring& val) {
            RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)val.c_str(),
                           (DWORD)((val.size() + 1) * sizeof(wchar_t)));
        };
        sz(L"DisplayName", kAppName);
        sz(L"DisplayVersion", kAppVersion);
        sz(L"Publisher", L"Zpix");
        sz(L"InstallLocation", dir);
        sz(L"DisplayIcon", exe);
        sz(L"UninstallString", L"\"" + uninst + L"\" /uninstall");
        sz(L"QuietUninstallString", L"\"" + uninst + L"\" /uninstall /silent");
        DWORD est = 43000;  // ~42 MB（含 PP-OCRv5 模型）
        RegSetValueExW(key, L"EstimatedSize", 0, REG_DWORD, (const BYTE*)&est, sizeof(est));
        DWORD v = 1;
        RegSetValueExW(key, L"NoModify", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegSetValueExW(key, L"NoRepair", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(key);
        return ERROR_SUCCESS;
    };
    LSTATUS rc = write(HKEY_LOCAL_MACHINE);
    if (rc != ERROR_SUCCESS) rc = write(HKEY_CURRENT_USER);
    return rc == ERROR_SUCCESS;
}

// 降权以普通桌面用户身份启动 Zpix.exe（避免安装器提权后带出高权限 Zpix 实例）
static void LaunchAsNormalUser(const std::wstring& exe, const wchar_t* extraArgs,
                               const std::wstring& workDir) {
    bool ok = false;
    std::wstring argsStr = (extraArgs && extraArgs[0]) ? std::wstring(extraArgs) : std::wstring();
    HWND shellWnd = GetShellWindow();
    if (shellWnd) {
        DWORD pid = 0;
        GetWindowThreadProcessId(shellWnd, &pid);
        if (pid) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                HANDLE hTok = nullptr;
                if (OpenProcessToken(hProc, TOKEN_DUPLICATE, &hTok)) {
                    HANDLE hPrim = nullptr;
                    if (DuplicateTokenEx(hTok,
                                         TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
                                             TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
                                         NULL, SecurityImpersonation, TokenPrimary, &hPrim)) {
                        STARTUPINFOW si{};
                        si.cb = sizeof(si);
                        PROCESS_INFORMATION pi{};
                        std::wstring cmd = L"\"" + exe + L"\"";
                        if (!argsStr.empty()) cmd += L" " + argsStr;
                        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
                        buf.push_back(0);
                        if (CreateProcessWithTokenW(hPrim, LOGON_WITH_PROFILE, exe.c_str(),
                                                    buf.data(), 0, NULL, workDir.c_str(), &si,
                                                    &pi)) {
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                            ok = true;
                            Log(L"launched normal-user %ls %ls", exe.c_str(), argsStr.c_str());
                        }
                        CloseHandle(hPrim);
                    }
                    CloseHandle(hTok);
                }
                CloseHandle(hProc);
            }
        }
    }
    if (!ok) {
        ShellExecuteW(NULL, L"open", exe.c_str(), argsStr.empty() ? NULL : argsStr.c_str(),
                      workDir.c_str(), SW_SHOWNORMAL);
        Log(L"launched fallback ShellExecute %ls %ls", exe.c_str(), argsStr.c_str());
    }
}

static std::wstring FormatFreeSpace(const std::wstring& dir) {
    std::wstring probe = dir;
    if (probe.size() >= 2 && probe[1] == L':') probe = probe.substr(0, 2) + L"\\";
    ULARGE_INTEGER avail{};
    if (!GetDiskFreeSpaceExW(probe.c_str(), &avail, NULL, NULL)) return L"需要约 42 MB 空间";
    double gb = (double)avail.QuadPart / (1024.0 * 1024.0 * 1024.0);
    wchar_t buf[96];
    swprintf(buf, 96, L"需要约 42 MB · 目标盘可用 %.1f GB", gb);
    return buf;
}

static bool PickFolderModern(HWND owner, const std::wstring& initial, std::wstring& out) {
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"选择 Zpix 安装目录");
        if (!initial.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), NULL, IID_PPV_ARGS(&item))) &&
                item) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        bool ok = false;
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* res = nullptr;
            if (SUCCEEDED(dlg->GetResult(&res)) && res) {
                PWSTR path = nullptr;
                if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    out = path;
                    CoTaskMemFree(path);
                    ok = true;
                }
                res->Release();
            }
        }
        dlg->Release();
        return ok;
    }

    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择 Zpix 安装目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return false;
    wchar_t buf[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(idl, buf) != FALSE;
    CoTaskMemFree(idl);
    if (ok) out = buf;
    return ok;
}

using ProgressFn = std::function<void(int, const wchar_t*)>;

static bool InstallSteps(const std::wstring& dir, bool desktop, bool menu, bool autoStart,
                         const ProgressFn& report, std::wstring* err) {
    std::wstring exe = dir + L"\\Zpix.exe";
    std::wstring ps1 = dir + L"\\ocr.ps1";
    std::wstring uninst = dir + L"\\uninstall.exe";
    std::wstring modelDir = dir + L"\\model";

    report(6, L"正在准备安装环境，关闭旧版 Zpix…");
    StopRunningApp(dir);
    if (!EnsureDir(dir) || !EnsureDir(modelDir)) {
        if (err) *err = L"无法创建目录 " + dir + L"，请更换位置或以管理员身份运行。";
        return false;
    }

    report(18, L"正在写入主程序 Zpix.exe…");
    if (!WriteResToFile(RES_ZPIX_EXE, exe, err)) return false;

    report(30, L"正在写入 OCR 辅助组件…");
    if (!WriteResToFile(RES_OCR_PS1, ps1, err)) return false;

    report(44, L"正在部署 ONNX 推理引擎…");
    if (!WriteResToFile(RES_ORT_DLL, modelDir + L"\\onnxruntime.dll", err)) return false;

    report(58, L"正在部署本地 PP-OCRv5 检测模型…");
    if (!WriteResToFile(RES_DET_ONNX, modelDir + L"\\det.onnx", err)) return false;

    report(74, L"正在部署本地 PP-OCRv5 识别模型…");
    if (!WriteResToFile(RES_REC_ONNX, modelDir + L"\\rec.onnx", err)) return false;
    if (!WriteResToFile(RES_KEYS_TXT, modelDir + L"\\ppocr_keys.txt", err)) return false;

    report(84, L"正在写入卸载程序…");
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (_wcsicmp(self, uninst.c_str()) != 0 && !CopyFileW(self, uninst.c_str(), FALSE)) {
        DWORD e = GetLastError();
        if (err) *err = L"无法写入卸载程序（错误码 " + std::to_wstring(e) + L"）";
        return false;
    }

    report(91, L"正在配置快捷方式与开机自启…");
    std::wstring desktopLnk = KnownDir(FOLDERID_Desktop) + L"\\Zpix 截图.lnk";
    std::wstring menuDir = KnownDir(FOLDERID_Programs) + L"\\Zpix";
    if (desktop) MakeLink(desktopLnk, exe, L"", dir, L"Zpix 截图 / 贴图 / 翻译 / 划词");
    else DeleteFileW(desktopLnk.c_str());

    if (menu) {
        MakeLink(menuDir + L"\\Zpix 截图.lnk", exe, L"", dir, L"Zpix 截图 / 贴图 / 翻译 / 划词");
        MakeLink(menuDir + L"\\卸载 Zpix.lnk", uninst, L"/uninstall", dir, L"卸载 Zpix 截图");
    } else {
        DeleteFileW((menuDir + L"\\Zpix 截图.lnk").c_str());
        DeleteFileW((menuDir + L"\\卸载 Zpix.lnk").c_str());
        RemoveDirectoryW(menuDir.c_str());
    }
    SetAutoStart(autoStart, exe);

    report(96, L"正在登记系统卸载信息…");
    SetUninstallEntry(dir, uninst);
    // 通知资源管理器立即刷新快捷方式图标缓存
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    report(100, L"安装完成");
    Log(L"installed ok -> %ls", dir.c_str());
    return true;
}

static bool UninstallSteps(const std::wstring& dir, const ProgressFn& report, std::wstring* err) {
    report(12, L"正在关闭运行中的 Zpix…");
    StopRunningApp(dir);
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(NULL, self, MAX_PATH);
    std::wstring uninst = dir + L"\\uninstall.exe";
    bool selfInside = (_wcsicmp(self, uninst.c_str()) == 0);

    report(32, L"正在移除快捷方式与开机自启…");
    DeleteFileW((KnownDir(FOLDERID_Desktop) + L"\\Zpix 截图.lnk").c_str());
    std::wstring menuDir = KnownDir(FOLDERID_Programs) + L"\\Zpix";
    DeleteFileW((menuDir + L"\\Zpix 截图.lnk").c_str());
    DeleteFileW((menuDir + L"\\卸载 Zpix.lnk").c_str());
    RemoveDirectoryW(menuDir.c_str());
    SetAutoStart(false, L"");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kRegUninstall);
    RegDeleteTreeW(HKEY_CURRENT_USER, kRegUninstall);

    report(60, L"正在删除主程序与本地 OCR 模型…");
    DeleteFileW((dir + L"\\Zpix.exe").c_str());
    DeleteFileW((dir + L"\\ocr.ps1").c_str());
    DeleteFileW((dir + L"\\ocr_out.txt").c_str());
    DeleteFileW((dir + L"\\zpix.log").c_str());
    DeleteFileW((dir + L"\\config.ini").c_str());
    std::wstring modelDir = dir + L"\\model";
    DeleteFileW((modelDir + L"\\onnxruntime.dll").c_str());
    DeleteFileW((modelDir + L"\\det.onnx").c_str());
    DeleteFileW((modelDir + L"\\rec.onnx").c_str());
    DeleteFileW((modelDir + L"\\ppocr_keys.txt").c_str());
    RemoveDirectoryW(modelDir.c_str());

    report(80, L"正在清理用户配置与缓存…");
    std::wstring userData = KnownDir(FOLDERID_LocalAppData) + L"\\Zpix";
    DeleteFileW((userData + L"\\zpix.log").c_str());
    DeleteFileW((userData + L"\\config.ini").c_str());
    DeleteFileW((userData + L"\\ocr.ps1").c_str());
    DeleteFileW((userData + L"\\ocr_out.txt").c_str());
    DeleteFileW((userData + L"\\ocr_tmp.png").c_str());
    DeleteFileW((userData + L"\\cli_result.txt").c_str());
    DeleteFileW((userData + L"\\datadir.txt").c_str());
    RemoveDirectoryW(userData.c_str());

    report(92, L"正在清理安装目录…");
    if (selfInside) {
        wchar_t tmpDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmpDir);
        SetCurrentDirectoryW(tmpDir);
        std::wstring cmd = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + uninst +
                           L"\" & rd /s /q \"" + dir + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, tmpDir, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            report(100, L"卸载完成");
            return true;
        }
        if (err) *err = L"无法清理卸载器自身，请手动删除目录：" + dir;
        return false;
    }
    DeleteFileW(uninst.c_str());
    RemoveDirectoryW(dir.c_str());
    report(100, L"卸载完成");
    Log(L"uninstalled ok -> %ls", dir.c_str());
    return true;
}

// ---------------------------------------------------------------- GDI+ 绘制原语

static Gdiplus::Color ToG(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

static void AddRoundPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.1f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void FillRound(HDC dc, RECT rc, int r, COLORREF fill, COLORREF border = CLR_INVALID) {
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::GraphicsPath path;
    AddRoundPath(path, (float)rc.left, (float)rc.top, (float)(rc.right - rc.left),
                 (float)(rc.bottom - rc.top), (float)r);
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush br(ToG(fill));
        gr.FillPath(&br, &path);
    }
    if (border != CLR_INVALID) {
        Gdiplus::Pen pen(ToG(border), 1.0f);
        gr.DrawPath(&pen, &path);
    }
}

static void DrawTxt(HDC dc, const std::wstring& txt, RECT rc, COLORREF col, HFONT f, UINT fmt) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, txt.c_str(), -1, &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void DrawCheckbox(HDC dc, RECT rc, const wchar_t* label, bool checked, bool hover) {
    int bs = S(16);
    int bx = rc.left;
    int by = (rc.top + rc.bottom - bs) / 2;
    RECT box{bx, by, bx + bs, by + bs};
    COLORREF fill = checked ? (hover ? kAccentHover : kAccent) : (hover ? kPanel : kBg);
    COLORREF bd = checked ? fill : (hover ? kAccent : RGB(196, 202, 212));
    FillRound(dc, box, S(4), fill, bd);

    if (checked) {
        Gdiplus::Graphics gr(dc);
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), std::max(1.6f, S(16) / 9.0f));
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::PointF pts[3] = {
            {(float)bx + bs * 0.24f, (float)by + bs * 0.52f},
            {(float)bx + bs * 0.44f, (float)by + bs * 0.72f},
            {(float)bx + bs * 0.78f, (float)by + bs * 0.30f},
        };
        gr.DrawLines(&pen, pts, 3);
    }

    RECT tr{bx + bs + S(8), rc.top, rc.right, rc.bottom};
    DrawTxt(dc, label, tr, hover ? kText : kSub, g.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void DrawButton(HDC dc, RECT rc, const wchar_t* text, bool primary, bool danger,
                       bool disabled, bool hover, bool pressed) {
    if (pressed && !disabled) OffsetRect(&rc, 0, 1);
    COLORREF fill, bd, fg;
    if (primary) {
        if (disabled) {
            fill = RGB(243, 206, 158);
            bd = CLR_INVALID;
            fg = RGB(255, 255, 255);
        } else if (danger) {
            fill = pressed ? RGB(185, 28, 28) : (hover ? RGB(205, 34, 34) : kRed);
            bd = CLR_INVALID;
            fg = RGB(255, 255, 255);
        } else {
            fill = pressed ? kAccentPress : (hover ? kAccentHover : kAccent);
            bd = CLR_INVALID;
            fg = RGB(255, 255, 255);
        }
    } else {
        fill = pressed ? RGB(236, 238, 243) : (hover ? kPanel : kBg);
        bd = hover ? RGB(196, 202, 212) : kBorder;
        fg = disabled ? kMuted : kText;
    }
    FillRound(dc, rc, S(9), fill, bd);
    DrawTxt(dc, text, rc, fg, g.fBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawAppBadgeIcon(HDC dc, int cx, int cy, int sz) {
    if (g.hIconBig) {
        DrawIconEx(dc, cx - sz / 2, cy - sz / 2, g.hIconBig, sz, sz, 0, NULL, DI_NORMAL);
    }
}

static void DrawStatusCircle(HDC dc, int cx, int cy, int sz, bool ok) {
    Gdiplus::Graphics gr(dc);
    gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush soft(ToG(ok ? kGreenSoft : kRedSoft));
    gr.FillEllipse(&soft, cx - sz / 2, cy - sz / 2, sz, sz);
    int inner = sz - S(14);
    Gdiplus::SolidBrush solid(ToG(ok ? kGreen : kRed));
    gr.FillEllipse(&solid, cx - inner / 2, cy - inner / 2, inner, inner);

    Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), std::max(2.6f, (float)S(3)));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (ok) {
        Gdiplus::PointF pts[3] = {
            {(float)cx - inner * 0.24f, (float)cy + inner * 0.02f},
            {(float)cx - inner * 0.05f, (float)cy + inner * 0.20f},
            {(float)cx + inner * 0.26f, (float)cy - inner * 0.17f},
        };
        gr.DrawLines(&pen, pts, 3);
    } else {
        gr.DrawLine(&pen, (float)cx, (float)cy - inner * 0.22f, (float)cx,
                    (float)cy + inner * 0.06f);
        Gdiplus::SolidBrush dot(Gdiplus::Color(255, 255, 255, 255));
        float dr = std::max(2.2f, S(2) * 1.1f);
        gr.FillEllipse(&dot, cx - dr, cy + inner * 0.16f - dr, dr * 2.0f, dr * 2.0f);
    }
}

// ---------------------------------------------------------------- 字体 & 布局

static void RebuildFonts() {
    HFONT* fs[] = {&g.fTitle, &g.fH1, &g.fH2, &g.fBody, &g.fSmall, &g.fBtn, &g.fPct};
    for (HFONT* f : fs) {
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
    }
    auto mk = [](int px, int w) {
        LOGFONTW lf{};
        lf.lfHeight = -S(px);
        lf.lfWeight = w;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy(lf.lfFaceName, L"Microsoft YaHei UI");
        return CreateFontIndirectW(&lf);
    };
    g.fTitle = mk(13, FW_SEMIBOLD);
    g.fH1 = mk(22, FW_BOLD);
    g.fH2 = mk(16, FW_SEMIBOLD);
    g.fBody = mk(13, FW_NORMAL);
    g.fSmall = mk(12, FW_NORMAL);
    g.fBtn = mk(14, FW_SEMIBOLD);
    g.fPct = mk(15, FW_BOLD);
    if (g.hEditDir) SendMessageW(g.hEditDir, WM_SETFONT, (WPARAM)g.fSmall, TRUE);
    if (g.hEditLic) SendMessageW(g.hEditLic, WM_SETFONT, (WPARAM)g.fSmall, TRUE);
}

static int DesiredHeight() {
    if (g.view == V_INSTALL && g.expanded) return S(518);
    if (g.view == V_LICENSE) return S(430);
    return S(372);
}

static void LayoutControls(bool resizeWindow) {
    int wantW = S(520);
    int wantH = DesiredHeight();
    if (resizeWindow && g.hwnd) {
        RECT cur{};
        GetWindowRect(g.hwnd, &cur);
        HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, &mi);
        int x = cur.left;
        int y = cur.top;
        if (x + wantW > mi.rcWork.right - S(8)) x = mi.rcWork.right - wantW - S(8);
        if (y + wantH > mi.rcWork.bottom - S(8)) y = mi.rcWork.bottom - wantH - S(8);
        if (x < mi.rcWork.left + S(8)) x = mi.rcWork.left + S(8);
        if (y < mi.rcWork.top + S(8)) y = mi.rcWork.top + S(8);
        SetWindowPos(g.hwnd, NULL, x, y, wantW, wantH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT cr{};
    GetClientRect(g.hwnd, &cr);
    g.W = cr.right;
    g.H = cr.bottom;

    // 标题栏按钮
    int bw = S(34), bh = S(28);
    g.rcClose = RECT{g.W - S(10) - bw, S(8), g.W - S(10), S(8) + bh};
    g.rcMin = RECT{g.rcClose.left - S(4) - bw, S(8), g.rcClose.left - S(4), S(8) + bh};

    // 清空视图相关命中区
    g.rcPrimary = g.rcSecondary = g.rcAgree = g.rcLicLink = g.rcExpand = RECT{};
    g.rcOptCard = g.rcDirBox = g.rcBrowse = RECT{};
    g.rcChkDesk = g.rcChkMenu = g.rcChkAuto = g.rcChkRun = RECT{};
    g.rcLicBox = g.rcLogBtn = RECT{};

    int pad = S(30);
    if (g.view == V_INSTALL) {
        int ctaW = S(216), ctaH = S(44);
        int ctaX = (g.W - ctaW) / 2;
        int ctaY = S(246);
        g.rcPrimary = RECT{ctaX, ctaY, ctaX + ctaW, ctaY + ctaH};

        int rowY = S(312);
        g.rcAgree = RECT{pad, rowY, pad + S(90), rowY + S(24)};
        g.rcLicLink = RECT{g.rcAgree.right, rowY, g.rcAgree.right + S(108), rowY + S(24)};
        g.rcExpand = RECT{g.W - pad - S(96), rowY, g.W - pad, rowY + S(24)};

        if (g.expanded) {
            int cardY = S(350);
            int cardH = S(142);
            g.rcOptCard = RECT{pad, cardY, g.W - pad, cardY + cardH};
            int ix = g.rcOptCard.left + S(16);
            int iw = g.rcOptCard.right - g.rcOptCard.left - S(32);
            int brW = S(80);
            g.rcDirBox = RECT{ix, cardY + S(36), ix + iw - brW - S(8), cardY + S(68)};
            g.rcBrowse = RECT{g.rcDirBox.right + S(8), cardY + S(36), ix + iw, cardY + S(68)};

            int half = iw / 2;
            int r1 = cardY + S(82);
            int r2 = cardY + S(110);
            g.rcChkDesk = RECT{ix, r1, ix + half - S(6), r1 + S(22)};
            g.rcChkMenu = RECT{ix + half + S(6), r1, ix + iw, r1 + S(22)};
            g.rcChkAuto = RECT{ix, r2, ix + half - S(6), r2 + S(22)};
            g.rcChkRun = RECT{ix + half + S(6), r2, ix + iw, r2 + S(22)};
        }
    } else if (g.view == V_LICENSE) {
        g.rcLicBox = RECT{pad, S(90), g.W - pad, g.H - S(78)};
        g.rcPrimary = RECT{g.W - pad - S(132), g.H - S(58), g.W - pad, g.H - S(20)};
    } else if (g.view == V_DONE) {
        if (g.success) {
            int ctaW = S(188), ctaH = S(42);
            g.rcPrimary = RECT{(g.W - ctaW) / 2, g.H - S(68), (g.W + ctaW) / 2, g.H - S(26)};
        } else {
            g.rcLogBtn = RECT{pad, g.H - S(64), pad + S(112), g.H - S(26)};
            g.rcPrimary = RECT{g.W - pad - S(116), g.H - S(64), g.W - pad, g.H - S(26)};
            g.rcSecondary = RECT{g.rcPrimary.left - S(10) - S(92), g.H - S(64),
                                 g.rcPrimary.left - S(10), g.H - S(26)};
        }
    } else if (g.view == V_UNINSTALL) {
        int bH = S(40), bY = g.H - S(64);
        g.rcPrimary = RECT{g.W - pad - S(124), bY, g.W - pad, bY + bH};
        g.rcSecondary = RECT{g.rcPrimary.left - S(10) - S(96), bY, g.rcPrimary.left - S(10),
                             bY + bH};
    }

    // 托管子 EDIT 控件
    if (g.hEditDir) {
        if (g.view == V_INSTALL && g.expanded) {
            SetWindowPos(g.hEditDir, HWND_TOP, g.rcDirBox.left + S(10), g.rcDirBox.top + S(7),
                         g.rcDirBox.right - g.rcDirBox.left - S(20),
                         g.rcDirBox.bottom - g.rcDirBox.top - S(14),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(g.hEditDir, SW_HIDE);
        }
    }
    if (g.hEditLic) {
        if (g.view == V_LICENSE) {
            SetWindowPos(g.hEditLic, HWND_TOP, g.rcLicBox.left + S(10), g.rcLicBox.top + S(8),
                         g.rcLicBox.right - g.rcLicBox.left - S(20),
                         g.rcLicBox.bottom - g.rcLicBox.top - S(16),
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(g.hEditLic, SW_HIDE);
        }
    }
}

static bool PtIn(const RECT& r, POINT p) {
    return r.right > r.left && r.bottom > r.top && p.x >= r.left && p.x < r.right && p.y >= r.top &&
           p.y < r.bottom;
}

static int HitTest(POINT p) {
    if (PtIn(g.rcClose, p)) return g.busy ? H_NONE : H_CLOSE;
    if (PtIn(g.rcMin, p)) return H_MIN;
    if (PtIn(g.rcPrimary, p)) return H_PRIMARY;
    if (PtIn(g.rcSecondary, p)) return H_SECONDARY;
    if (PtIn(g.rcAgree, p)) return H_AGREE;
    if (PtIn(g.rcLicLink, p)) return H_LICENSE;
    if (PtIn(g.rcExpand, p)) return H_EXPAND;
    if (PtIn(g.rcBrowse, p)) return H_BROWSE;
    if (PtIn(g.rcChkDesk, p)) return H_CHK_DESK;
    if (PtIn(g.rcChkMenu, p)) return H_CHK_MENU;
    if (PtIn(g.rcChkAuto, p)) return H_CHK_AUTO;
    if (PtIn(g.rcChkRun, p)) return H_CHK_RUN;
    if (PtIn(g.rcLogBtn, p)) return H_LOG;
    return H_NONE;
}

// ---------------------------------------------------------------- 页面渲染（双缓冲零闪烁）

static void PaintWindow(HDC dc) {
    RECT all{0, 0, g.W, g.H};
    FillRect(dc, &all, g.brWhite);

    // 细外框（在无边框 Win10/11 上都保持精致边界）
    {
        HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, 0, 0, g.W, g.H);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    // 自定义标题栏：左上小图标 + 标题，右上最小化/关闭
    if (g.hIconSm) DrawIconEx(dc, S(16), S(14), g.hIconSm, S(16), S(16), 0, NULL, DI_NORMAL);
    DrawTxt(dc, g.uninstallMode ? L"卸载 Zpix" : L"Zpix 安装程序",
            RECT{S(38), S(10), S(260), S(34)}, kSub, g.fTitle,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 最小化按钮
    {
        if (g.hover == H_MIN) FillRound(dc, g.rcMin, S(6), RGB(240, 242, 246));
        int mx = (g.rcMin.left + g.rcMin.right) / 2;
        int my = (g.rcMin.top + g.rcMin.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, g.hover == H_MIN ? kText : kSub);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, mx - S(5), my, NULL);
        LineTo(dc, mx + S(6), my);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
    // 关闭按钮
    {
        bool hov = (g.hover == H_CLOSE) && !g.busy;
        if (hov) FillRound(dc, g.rcClose, S(6), RGB(239, 68, 68));
        int cx = (g.rcClose.left + g.rcClose.right) / 2;
        int cy = (g.rcClose.top + g.rcClose.bottom) / 2;
        int arm = S(4);
        COLORREF col = g.busy ? RGB(205, 209, 216) : (hov ? RGB(255, 255, 255) : kSub);
        HPEN pen = CreatePen(PS_SOLID, 1, col);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, cx - arm, cy - arm, NULL);
        LineTo(dc, cx + arm + 1, cy + arm + 1);
        MoveToEx(dc, cx + arm, cy - arm, NULL);
        LineTo(dc, cx - arm - 1, cy + arm + 1);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    int pad = S(30);
    if (g.view == V_INSTALL) {
        DrawAppBadgeIcon(dc, g.W / 2, S(86), S(62));

        // 标题 + 版本徽章
        std::wstring title = kAppName;
        std::wstring badge = g.isUpgrade ? (std::wstring(L"v") + kAppVersion + L" · 升级")
                                         : (std::wstring(L"v") + kAppVersion);
        SIZE tsz{}, bsz{};
        HGDIOBJ of = SelectObject(dc, g.fH1);
        GetTextExtentPoint32W(dc, title.c_str(), (int)title.size(), &tsz);
        SelectObject(dc, g.fSmall);
        GetTextExtentPoint32W(dc, badge.c_str(), (int)badge.size(), &bsz);
        SelectObject(dc, of);
        int bW = bsz.cx + S(16);
        int totalW = tsz.cx + S(10) + bW;
        int sx = (g.W - totalW) / 2;
        DrawTxt(dc, title, RECT{sx, S(130), sx + tsz.cx + S(4), S(162)}, kText, g.fH1,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT brc{sx + tsz.cx + S(10), S(136), sx + tsz.cx + S(10) + bW, S(158)};
        FillRound(dc, brc, S(11), kAccentSoft, RGB(248, 212, 162));
        DrawTxt(dc, badge, brc, kAccentHover, g.fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        DrawTxt(dc, kAppTagline, RECT{pad, S(168), g.W - pad, S(190)}, kSub, g.fBody,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 三个核心特性胶囊标签
        const wchar_t* chips[3] = {L"本地 PP-OCRv5", L"智谱 / BliBli / Google", L"全局划词翻译"};
        int chipW[3]{}, sumW = 0;
        of = SelectObject(dc, g.fSmall);
        for (int i = 0; i < 3; ++i) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, chips[i], (int)wcslen(chips[i]), &sz);
            chipW[i] = sz.cx + S(18);
            sumW += chipW[i];
        }
        SelectObject(dc, of);
        int gap = S(8);
        int cx = (g.W - (sumW + gap * 2)) / 2;
        for (int i = 0; i < 3; ++i) {
            RECT cr{cx, S(200), cx + chipW[i], S(224)};
            FillRound(dc, cr, S(12), kPanel, kBorder);
            DrawTxt(dc, chips[i], cr, kSub, g.fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            cx += chipW[i] + gap;
        }

        // 主 CTA 按钮
        DrawButton(dc, g.rcPrimary, g.isUpgrade ? L"立即升级" : L"立即安装", true, false,
                   !g.agreed, g.hover == H_PRIMARY, g.pressed == H_PRIMARY);

        // 协议勾选 + 许可链接 + 自定义展开按钮
        DrawCheckbox(dc, g.rcAgree, L"我已同意", g.agreed, g.hover == H_AGREE);
        DrawTxt(dc, L"《MIT 许可协议》", g.rcLicLink,
                g.hover == H_LICENSE ? kAccentHover : kAccent, g.fSmall,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        std::wstring expLbl = g.expanded ? L"收起选项 ▴" : L"自定义选项 ▾";
        DrawTxt(dc, expLbl, g.rcExpand, g.hover == H_EXPAND ? kAccent : kSub, g.fSmall,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        if (g.expanded) {
            FillRound(dc, g.rcOptCard, S(12), kPanel, kBorder);
            int ix = g.rcOptCard.left + S(16);
            int topY = g.rcOptCard.top + S(12);
            DrawTxt(dc, L"安装目录", RECT{ix, topY, ix + S(80), topY + S(18)}, kText, g.fSmall,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawTxt(dc, FormatFreeSpace(g.dir),
                    RECT{ix + S(84), topY, g.rcOptCard.right - S(16), topY + S(18)}, kMuted,
                    g.fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            FillRound(dc, g.rcDirBox, S(7), kBg, kBorder);
            DrawButton(dc, g.rcBrowse, L"更改目录", false, false, false, g.hover == H_BROWSE,
                       g.pressed == H_BROWSE);

            DrawCheckbox(dc, g.rcChkDesk, L"创建桌面快捷方式", g.optDesktop,
                         g.hover == H_CHK_DESK);
            DrawCheckbox(dc, g.rcChkMenu, L"创建开始菜单快捷方式", g.optMenu,
                         g.hover == H_CHK_MENU);
            DrawCheckbox(dc, g.rcChkAuto, L"开机自动启动 Zpix", g.optAutoStart,
                         g.hover == H_CHK_AUTO);
            DrawCheckbox(dc, g.rcChkRun, L"安装完成后立即启动", g.optLaunch,
                         g.hover == H_CHK_RUN);
        }
    } else if (g.view == V_LICENSE) {
        DrawTxt(dc, L"MIT 许可协议", RECT{pad, S(54), g.W - pad, S(80)}, kText, g.fH2,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        FillRound(dc, g.rcLicBox, S(10), kBg, kBorder);
        DrawButton(dc, g.rcPrimary, L"我已了解，返回", true, false, false, g.hover == H_PRIMARY,
                   g.pressed == H_PRIMARY);
    } else if (g.view == V_PROGRESS) {
        DrawAppBadgeIcon(dc, g.W / 2, S(104), S(54));
        const wchar_t* headline = g.uninstallMode
                                      ? L"正在卸载 Zpix…"
                                      : (g.isUpgrade ? L"正在升级 Zpix…" : L"正在安装 Zpix…");
        DrawTxt(dc, headline, RECT{pad, S(156), g.W - pad, S(188)}, kText, g.fH2,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // 平滑橙色渐变进度条（GDI+ 抗锯齿）
        RECT bar{pad + S(10), S(214), g.W - pad - S(10), S(226)};
        int barH = bar.bottom - bar.top;
        FillRound(dc, bar, barH / 2, RGB(236, 239, 245));
        float pct = std::max(0.0f, std::min(100.0f, g.dispPct));
        int fillW = (int)std::round((bar.right - bar.left) * pct / 100.0f);
        if (fillW > barH / 2) {
            Gdiplus::Graphics gr(dc);
            gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::GraphicsPath fp;
            AddRoundPath(fp, (float)bar.left, (float)bar.top, (float)fillW, (float)barH,
                         barH * 0.5f);
            Gdiplus::LinearGradientBrush lg(
                Gdiplus::Point(bar.left, bar.top), Gdiplus::Point(bar.right, bar.top),
                Gdiplus::Color(255, 248, 168, 52), Gdiplus::Color(255, 234, 134, 14));
            gr.FillPath(&lg, &fp);
        }

        wchar_t pctBuf[16];
        swprintf(pctBuf, 16, L"%d%%", (int)std::round(pct));
        DrawTxt(dc, g.statusText, RECT{bar.left, S(240), bar.right - S(58), S(264)}, kSub, g.fSmall,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTxt(dc, pctBuf, RECT{bar.right - S(56), S(238), bar.right, S(266)}, kAccent, g.fPct,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        DrawTxt(dc, L"请稍候，完成后将自动就绪…", RECT{pad, S(296), g.W - pad, S(318)}, kMuted,
                g.fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else if (g.view == V_DONE) {
        DrawStatusCircle(dc, g.W / 2, S(96), S(58), g.success);
        const wchar_t* head =
            g.uninstallMode
                ? (g.success ? L"卸载完成" : L"卸载未完成")
                : (g.success ? (g.isUpgrade ? L"升级完成" : L"安装完成") : L"安装失败");
        DrawTxt(dc, head, RECT{pad, S(140), g.W - pad, S(172)}, kText, g.fH1,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        if (g.success) {
            RECT card{pad + S(8), S(186), g.W - pad - S(8), S(278)};
            FillRound(dc, card, S(12), kPanel, kBorder);
            if (g.uninstallMode) {
                DrawTxt(dc, L"Zpix 及其本地 OCR 模型、快捷方式与开机自启均已清理干净。",
                        RECT{card.left + S(16), card.top + S(18), card.right - S(16),
                             card.bottom - S(16)},
                        kSub, g.fBody, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            } else {
                DrawTxt(dc, L"安装位置：" + g.dir,
                        RECT{card.left + S(16), card.top + S(16), card.right - S(16),
                             card.top + S(38)},
                        kText, g.fSmall,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
                std::wstring tip = g.launched
                                       ? L"Zpix 已在后台启动：F1 截图 · F2 贴图 · 划选文字自动触发翻译"
                                       : L"快捷方式已创建：F1 截图 · F2 贴图 · 划选文字自动触发翻译";
                DrawTxt(dc, tip,
                        RECT{card.left + S(16), card.top + S(46), card.right - S(16),
                             card.top + S(74)},
                        kSub, g.fSmall, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            }
            DrawButton(dc, g.rcPrimary, L"完成", true, false, false, g.hover == H_PRIMARY,
                       g.pressed == H_PRIMARY);
        } else {
            RECT card{pad, S(186), g.W - pad, S(282)};
            FillRound(dc, card, S(10), kRedSoft, RGB(252, 165, 165));
            DrawTxt(dc, g.failReason,
                    RECT{card.left + S(14), card.top + S(12), card.right - S(14),
                         card.bottom - S(12)},
                    kRed, g.fSmall, DT_LEFT | DT_WORDBREAK);
            DrawButton(dc, g.rcLogBtn, L"查看安装日志", false, false, false, g.hover == H_LOG,
                       g.pressed == H_LOG);
            DrawButton(dc, g.rcSecondary, L"关闭", false, false, false, g.hover == H_SECONDARY,
                       g.pressed == H_SECONDARY);
            DrawButton(dc, g.rcPrimary, L"重试", true, false, false, g.hover == H_PRIMARY,
                       g.pressed == H_PRIMARY);
        }
    } else if (g.view == V_UNINSTALL) {
        DrawAppBadgeIcon(dc, g.W / 2, S(92), S(54));
        DrawTxt(dc, L"确定要卸载 Zpix 截图吗？", RECT{pad, S(140), g.W - pad, S(170)}, kText, g.fH2,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT card{pad, S(184), g.W - pad, S(280)};
        FillRound(dc, card, S(12), kPanel, kBorder);
        DrawTxt(dc, L"目标目录：" + g.dir,
                RECT{card.left + S(16), card.top + S(16), card.right - S(16), card.top + S(38)},
                kText, g.fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        DrawTxt(dc, L"将彻底移除主程序、本地 PP-OCRv5 模型、快捷方式、开机自启项及配置文件。",
                RECT{card.left + S(16), card.top + S(44), card.right - S(16), card.bottom - S(12)},
                kSub, g.fSmall, DT_LEFT | DT_WORDBREAK);
        DrawButton(dc, g.rcSecondary, L"取消", false, false, false, g.hover == H_SECONDARY,
                   g.pressed == H_SECONDARY);
        DrawButton(dc, g.rcPrimary, L"确认卸载", true, true, false, g.hover == H_PRIMARY,
                   g.pressed == H_PRIMARY);
    }
}

// ---------------------------------------------------------------- 后台任务触发

static void SwitchView(View v) {
    g.view = v;
    g.hover = H_NONE;
    g.pressed = H_NONE;
    LayoutControls(true);
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static void StartWorker(bool isUninstall) {
    if (g.busy) return;
    if (!isUninstall && g.hEditDir) {
        wchar_t buf[MAX_PATH] = {};
        GetWindowTextW(g.hEditDir, buf, MAX_PATH);
        if (buf[0]) g.dir = buf;
    }
    g.busy = true;
    g.success = false;
    g.launched = false;
    g.failReason.clear();
    g.targetPct = 4;
    g.dispPct = 0.0f;
    g.statusText = isUninstall ? L"正在准备卸载…" : L"正在准备安装…";
    SwitchView(V_PROGRESS);
    SetTimer(g.hwnd, kAnimTimer, 16, NULL);

    std::wstring dir = g.dir;
    bool desk = g.optDesktop, menu = g.optMenu, aut = g.optAutoStart;
    HWND targetWnd = g.hwnd;

    std::thread([=]() {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        auto postProg = [targetWnd](int pct, const wchar_t* msg) {
            if (targetWnd && IsWindow(targetWnd)) {
                PostMessageW(targetWnd, WM_SETUP_PROGRESS, (WPARAM)pct,
                             (LPARAM) new std::wstring(msg));
            }
        };
        std::wstring err;
        bool ok = isUninstall ? UninstallSteps(dir, postProg, &err)
                              : InstallSteps(dir, desk, menu, aut, postProg, &err);
        if (targetWnd && IsWindow(targetWnd)) {
            PostMessageW(targetWnd, WM_SETUP_DONE, ok ? 1 : 0, (LPARAM) new std::wstring(err));
        }
        CoUninitialize();
    }).detach();
}

static LRESULT CALLBACK SetupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        UINT d = GetDpiForWindow(hwnd);
        g.dpi = d ? (int)d : 96;
        // Win11 原生圆角与阴影
        typedef HRESULT(WINAPI * PFN_Dwm)(HWND, DWORD, LPCVOID, DWORD);
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
            auto pfn = (PFN_Dwm)GetProcAddress(dwm, "DwmSetWindowAttribute");
            if (pfn) {
                int pref = 2;  // DWMWCP_ROUND
                pfn(hwnd, 33, &pref, sizeof(pref));
            }
        }
        return 0;
    }
    case WM_DPICHANGED: {
        UINT d = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
        if (d >= 96) {
            g.dpi = (int)d;
            RebuildFonts();
            LayoutControls(true);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
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
        PaintWindow(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, kText);
        SetBkColor(dc, kBg);
        return (LRESULT)g.brWhite;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 2001 && HIWORD(wp) == EN_CHANGE && g.hEditDir) {
            wchar_t buf[MAX_PATH] = {};
            GetWindowTextW(g.hEditDir, buf, MAX_PATH);
            g.dir = buf;
            InvalidateRect(hwnd, &g.rcOptCard, FALSE);
        }
        return 0;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (HitTest(pt) == H_NONE && !PtIn(g.rcDirBox, pt) && !PtIn(g.rcLicBox, pt))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int hit = HitTest(pt);
            bool disabled = (hit == H_PRIMARY && g.view == V_INSTALL && !g.agreed);
            SetCursor(LoadCursorW(NULL, (hit != H_NONE && !disabled) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int h = HitTest(pt);
        if (h != g.hover) {
            g.hover = h;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (!g.tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g.tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g.tracking = false;
        g.hover = H_NONE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g.pressed = HitTest(pt);
        if (g.pressed != H_NONE) SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (GetCapture() == hwnd) ReleaseCapture();
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = HitTest(pt);
        int prs = g.pressed;
        g.pressed = H_NONE;
        InvalidateRect(hwnd, NULL, FALSE);
        if (hit == H_NONE || hit != prs) return 0;

        switch (hit) {
        case H_MIN:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case H_CLOSE:
            if (!g.busy) DestroyWindow(hwnd);
            break;
        case H_AGREE:
            g.agreed = !g.agreed;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case H_LICENSE:
            SwitchView(V_LICENSE);
            break;
        case H_EXPAND:
            g.expanded = !g.expanded;
            LayoutControls(true);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case H_BROWSE: {
            std::wstring picked;
            if (PickFolderModern(hwnd, g.dir, picked) && !picked.empty()) {
                if (picked.find(L"Zpix") == std::wstring::npos &&
                    GetFileAttributesW((picked + L"\\Zpix.exe").c_str()) ==
                        INVALID_FILE_ATTRIBUTES) {
                    if (picked.back() != L'\\') picked += L'\\';
                    picked += L"Zpix";
                }
                g.dir = picked;
                if (g.hEditDir) SetWindowTextW(g.hEditDir, g.dir.c_str());
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;
        }
        case H_CHK_DESK:
            g.optDesktop = !g.optDesktop;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case H_CHK_MENU:
            g.optMenu = !g.optMenu;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case H_CHK_AUTO:
            g.optAutoStart = !g.optAutoStart;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case H_CHK_RUN:
            g.optLaunch = !g.optLaunch;
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case H_LOG:
            ShellExecuteW(NULL, L"open", L"notepad.exe", LogFilePath().c_str(), NULL,
                          SW_SHOWNORMAL);
            break;
        case H_SECONDARY:
            DestroyWindow(hwnd);
            break;
        case H_PRIMARY:
            if (g.view == V_INSTALL) {
                if (g.agreed) StartWorker(false);
            } else if (g.view == V_LICENSE) {
                SwitchView(V_INSTALL);
            } else if (g.view == V_UNINSTALL) {
                StartWorker(true);
            } else if (g.view == V_DONE) {
                if (g.success)
                    DestroyWindow(hwnd);
                else
                    StartWorker(g.uninstallMode);
            }
            break;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kAnimTimer) {
            float diff = (float)g.targetPct - g.dispPct;
            if (std::fabs(diff) < 0.4f) {
                g.dispPct = (float)g.targetPct;
                if (!g.busy) KillTimer(hwnd, kAnimTimer);
            } else {
                g.dispPct += diff * 0.22f;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    case WM_SETUP_PROGRESS: {
        std::wstring* s = (std::wstring*)lp;
        if ((int)wp > g.targetPct) g.targetPct = (int)wp;
        if (s) {
            g.statusText = *s;
            delete s;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_SETUP_DONE: {
        std::wstring* err = (std::wstring*)lp;
        g.busy = false;
        g.success = (wp != 0);
        g.targetPct = 100;
        g.dispPct = 100.0f;
        KillTimer(hwnd, kAnimTimer);
        if (err) {
            g.failReason = *err;
            delete err;
        }
        if (!g.success && g.failReason.empty()) g.failReason = L"操作未完成，请查看日志。";
        if (g.success && !g.uninstallMode && g.optLaunch) {
            LaunchAsNormalUser(g.dir + L"\\Zpix.exe", L"--after-install", g.dir);
            g.launched = true;
        }
        SwitchView(V_DONE);
        return 0;
    }
    case WM_CLOSE:
        if (g.busy) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kAnimTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- CLI & 入口

static std::wstring ArgValue(const wchar_t* flag) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring out;
    for (int i = 1; i < argc - 1; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) {
            out = argv[i + 1];
            break;
        }
    }
    LocalFree(argv);
    return out;
}

static bool HasFlag(const wchar_t* flag) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], flag) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

static bool IsSelfNamedUninstall() {
    wchar_t self[MAX_PATH] = {};
    if (!GetModuleFileNameW(NULL, self, MAX_PATH)) return false;
    const wchar_t* base = PathFindFileNameW(self);
    if (!base) return false;
    return _wcsnicmp(base, L"uninst", 6) == 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    g.inst = inst;
    typedef BOOL(WINAPI * PFN_Ctx)(HANDLE);
    if (HMODULE u32 = LoadLibraryW(L"user32.dll")) {
        auto pfn = (PFN_Ctx)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (pfn) pfn((HANDLE)-4);
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    HANDLE mutex = CreateMutexW(NULL, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND prev = FindWindowW(L"ZpixSetupWnd", NULL)) {
            ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
        }
        return 0;
    }

    g.silent = HasFlag(L"/silent") || HasFlag(L"/S");
    g.uninstallMode = HasFlag(L"/uninstall") || HasFlag(L"-uninstall") || HasFlag(L"--uninstall") ||
                      IsSelfNamedUninstall();
    if (g.uninstallMode) {
        wchar_t tmpDir[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tmpDir)) SetCurrentDirectoryW(tmpDir);
    }
    std::wstring existing = QueryInstalledDir();
    g.isUpgrade = !existing.empty() &&
                  GetFileAttributesW((existing + L"\\Zpix.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    g.dir = ArgValue(L"/dir");
    if (g.dir.empty()) {
        if (g.uninstallMode)
            g.dir = UninstallTargetDir();
        else
            g.dir = g.isUpgrade ? existing : DefaultInstallDir();
    }
    g.optAutoStart = g.isUpgrade ? HasAutoStart() : true;

    Log(L"setup start uninstall=%d upgrade=%d silent=%d dir=%ls", (int)g.uninstallMode,
        (int)g.isUpgrade, (int)g.silent, g.dir.c_str());

    if (g.silent) {
        auto noop = [](int, const wchar_t*) {};
        bool ok = g.uninstallMode
                      ? UninstallSteps(g.dir, noop, nullptr)
                      : InstallSteps(g.dir, true, true, g.optAutoStart, noop, nullptr);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&g.gdiToken, &gsi, NULL);

    g.brWhite = CreateSolidBrush(kBg);
    g.hIconBig = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 128, 128, LR_DEFAULTCOLOR);
    g.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 24, 24, LR_DEFAULTCOLOR);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SetupProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"ZpixSetupWnd";
    wc.hIcon = g.hIconBig;
    wc.hIconSm = g.hIconSm;
    wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);

    HDC scr = GetDC(NULL);
    int sysDpi = GetDeviceCaps(scr, LOGPIXELSY);
    ReleaseDC(NULL, scr);
    g.dpi = sysDpi >= 96 ? sysDpi : 96;

    int initW = S(520), initH = S(372);
    g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"ZpixSetupWnd",
                             g.uninstallMode ? L"卸载 Zpix 截图" : L"安装 Zpix 截图",
                             WS_POPUP | WS_MINIMIZEBOX | WS_CLIPCHILDREN, 0, 0, initW, initH, NULL,
                             NULL, inst, NULL);
    if (!g.hwnd) return 1;

    g.hEditDir = CreateWindowExW(0, L"EDIT", g.dir.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10,
                                 10, g.hwnd, (HMENU)(INT_PTR)2001, inst, NULL);
    g.hEditLic = CreateWindowExW(0, L"EDIT", kLicenseText,
                                 WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                     ES_AUTOVSCROLL,
                                 0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)2002, inst, NULL);
    RebuildFonts();

    g.view = g.uninstallMode ? V_UNINSTALL : V_INSTALL;
    std::wstring pv = ArgValue(L"/page");
    if (!pv.empty()) {
        int n = _wtoi(pv.c_str());
        if (n >= V_INSTALL && n <= V_UNINSTALL) g.view = (View)n;
        if (HasFlag(L"/expand")) g.expanded = true;
    }

    // 主屏居中
    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int h = DesiredHeight();
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - initW) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
    SetWindowPos(g.hwnd, NULL, x, y, initW, h, SWP_NOZORDER);
    LayoutControls(false);

    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g.brWhite) DeleteObject(g.brWhite);
    if (g.hIconBig) DestroyIcon(g.hIconBig);
    if (g.hIconSm) DestroyIcon(g.hIconSm);
    if (g.gdiToken) Gdiplus::GdiplusShutdown(g.gdiToken);
    CoUninitialize();
    return 0;
}
