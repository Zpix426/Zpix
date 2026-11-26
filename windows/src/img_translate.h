#pragma once
// 图内原位翻译：本地 PP-OCRv5 自然段检测 + 批量翻译 + 背景采样擦除 + 原位自适应中文排版
#include "util.h"
#include <string>

namespace img_translate {

struct Result {
    Bitmap32 image;             // 合成译文后的选区位图（尺寸与输入选区一致）
    int blockCount = 0;         // 成功替换的自然段/词块数
    int ocrMs = 0;              // 本地 OCR 耗时
    int trMs = 0;               // 翻译网络耗时
    std::wstring provider;      // 命中的翻译服务名称
    std::wstring targetLang;    // 目标语言代码
    std::wstring error;         // 失败或无文字时的提示语
};

// 对已裁剪出的选区位图执行图内原位翻译（同步函数，供后台线程或命令行自测调用）
bool TranslateSelectionImage(const Bitmap32& srcSel, Result& out);

// 从本地图片文件读取、执行图内原位翻译并保存结果 PNG（供命令行自测与快速验证使用）
bool TranslateImageFile(const std::wstring& inPath, const std::wstring& outPngPath, Result& out);

}  // namespace img_translate
