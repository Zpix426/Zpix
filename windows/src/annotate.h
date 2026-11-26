#pragma once
#include "util.h"
#include <vector>

enum class AnnType { Rect, Arrow, Text, Mosaic };

struct Ann {
    AnnType type = AnnType::Rect;
    POINT a{};              // 起点（屏幕坐标）
    POINT b{};              // 终点（屏幕坐标）
    std::wstring text;      // 文本工具
    COLORREF color = RGB(239, 68, 68);
    int width = 4;          // 线宽（2 / 4 / 6）
    int fontSize = 22;      // 文本字号（16 / 22 / 32）
    int block = 10;         // 马赛克块大小
    Bitmap32 patch;         // 马赛克预览缓存（屏幕坐标区域，左上角 = a）
    bool cached = false;
};

// 在 dc 上按屏幕坐标绘制（offset 用于导出时平移）
void DrawAnnotations(HDC dc, const std::vector<Ann>& items, POINT offset);

// 把 shot 的 sel 区域 + 标注合成为一张图
Bitmap32 RenderSelection(const Bitmap32& shot, RECT sel, const std::vector<Ann>& items);

// 基于已裁剪/已替换底图的选区位图叠加标注合成（供图内原位翻译后复制/贴图/保存使用）
Bitmap32 RenderSelectionFromCrop(const Bitmap32& baseCrop, RECT sel, const std::vector<Ann>& items);

// 生成马赛克预览块（从 shot 取 a..b 区域并像素化）
Bitmap32 MakeMosaicPatch(const Bitmap32& shot, POINT a, POINT b, int block);

// 在 Bitmap32 内部对局部矩形做马赛克（坐标为图片坐标）
void ApplyMosaic(Bitmap32& bm, RECT local, int block);

RECT NormalizePointRect(POINT a, POINT b);