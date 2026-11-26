#pragma once
#include <windows.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------- 设计变量
// 纯白底 + 极浅灰分区 + 柔和橙强调色（Linear / Raycast 风格）
namespace ui {

inline COLORREF Bg() { return RGB(255, 255, 255); }         // #FFFFFF
inline COLORREF Panel() { return RGB(247, 248, 250); }      // #F7F8FA
inline COLORREF PanelHover() { return RGB(242, 244, 247); } // 卡片悬停
inline COLORREF Accent() { return RGB(240, 144, 21); }      // #F09015
inline COLORREF AccentSoft() { return RGB(255, 247, 236); } // 选中态浅橙底
inline COLORREF AccentHover() { return RGB(247, 158, 45); }
inline COLORREF Text() { return RGB(37, 37, 37); }          // #252525
inline COLORREF SubText() { return RGB(138, 138, 138); }    // #8A8A8A
inline COLORREF Border() { return RGB(229, 231, 235); }     // #E5E7EB
inline COLORREF Hairline() { return RGB(239, 241, 244); }   // 分隔线
inline COLORREF Field() { return RGB(255, 255, 255); }      // 输入框底
inline COLORREF TrackOff() { return RGB(216, 220, 227); }   // 开关关闭

// 圆角矩形（GDI+ 抗锯齿）；fill 可为 CLR_INVALID 表示只描边
void RoundRect(HDC dc, RECT rc, int radius, COLORREF fill, COLORREF border, int borderWidth = 1);

// 文字：font 由调用方 Selected；align 支持 DT_* 组合
void Text(HDC dc, const std::wstring& s, RECT rc, COLORREF color, HFONT font, UINT flags);

// 单行文字（垂直居中）
void TextLine(HDC dc, const std::wstring& s, int x, int y, int w, int h, COLORREF color, HFONT font,
              UINT flags = DT_LEFT);

// 自定义开关（36x20，橙色开启）
void Toggle(HDC dc, RECT rc, float t /*0..1*/, bool hover);

// 图标：22px 应用 logo 从 HICON 绘制
void DrawIcon22(HDC dc, HICON icon, int x, int y, int size);

// 自绘现代圆角下拉菜单项（id = -1 表示分隔线）
struct DropdownItem {
    int id = 0;
    std::wstring label;
    std::wstring sub;
    bool checked = false;
};

// 在触发按钮屏幕矩形 anchorScreen 下方/上方弹出自绘圆角下拉菜单；
// clampScreen 非空时自动约束在父窗口宽度内（防右侧超出）；取消返回 -1。
int ShowDropdownPopup(HWND owner, RECT anchorScreen, const std::vector<DropdownItem>& items,
                      int scale = 100, const RECT* clampScreen = nullptr);

}  // namespace ui

// 灰阶/彩色插值（动画用）
inline COLORREF MixColor(COLORREF a, COLORREF b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, bl);
}