#pragma once
#include <windows.h>
#include <windowsx.h>
#include <vector>
#include <string>
#include <cstdint>

// 32bit top-down ARGB(内存里是 BGRA, 用 0xAARRGGBB 表示)
struct Bitmap32 {
    int w = 0;
    int h = 0;
    std::vector<uint32_t> px;
    bool empty() const { return w <= 0 || h <= 0 || px.empty(); }
    uint32_t* row(int y) { return px.data() + (size_t)y * w; }
    const uint32_t* row(int y) const { return px.data() + (size_t)y * w; }
};

RECT GetVirtualScreenRect();

// 单显示器工作区：只返回"某一块屏"的 rcWork，绝不会把两块屏拼成一个大矩形。
// 多屏下窗口定位必须用这个，否则居中会落在两屏中间的缝上（一边一半）。
RECT MonitorWorkRect(POINT pt);   // pt 所在屏；不在任何屏内则取最近的屏
int MonitorScaleOf(POINT pt);     // 同一块屏的缩放百分比（100 / 125 / 150…）
RECT PrimaryMonitorWorkRect();    // 主屏工作区（设置窗口居中用）

void EnableDpiAwareness();
bool InitGdiPlus();
void ShutdownGdiPlus();

bool CaptureVirtualScreen(Bitmap32& out);
Bitmap32 CropBitmap(const Bitmap32& src, RECT rScreen);

bool SavePngFile(const Bitmap32& bm, const std::wstring& path);
bool CopyToClipboard(HWND owner, const Bitmap32& bm);

std::wstring AppDataSaveDir();
std::wstring NormalizeDirPath(const std::wstring& dir);
bool EnsureDirectoryRecursive(const std::wstring& dir);
std::wstring EffectiveSaveRootDir(const std::wstring& configuredDir);
std::wstring TodayDateFolderName();
std::wstring BuildDailyShotPath(const std::wstring& configuredDir, std::wstring* outDateDir = nullptr);
std::wstring PreviewDailyShotPath(const std::wstring& configuredDir);
void OpenFolderInExplorer(const std::wstring& dir);
void OpenFolderAndSelectFile(const std::wstring& filePath, const std::wstring& fallbackDir);
bool PickFolderDialog(HWND owner, const std::wstring& initialDir, std::wstring& outDir);

std::wstring ExeDirPath();   // exe 目录，带尾随反斜杠
std::wstring DataDir();      // 可写数据目录（exe 旁不可写时退回 %LOCALAPPDATA%\Zpix\）
void EnsureDataFiles();      // 只读安装目录首次运行时迁移 config.ini / ocr.ps1
std::wstring TimestampFileName();

HFONT MakeUiFont(int px, bool bold = false);
void DrawTextShadow(HDC dc, const std::wstring& text, RECT rc, HFONT font,
                    COLORREF color, UINT fmt);

// 把 Bitmap32 变成可用于 BitBlt/StretchBlt 的 DIB 内存 DC（top-down）
HDC CreateBitmapDC(const Bitmap32& bm, HBITMAP& outBmp, HGDIOBJ& oldObj);

// 简易日志：%TEMP%\zpix.log
void ZLog(const wchar_t* fmt, ...);