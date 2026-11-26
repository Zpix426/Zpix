#pragma once
// PP-OCRv5 本地识别（onnxruntime）：det 检测 + DB 后处理 + rec 识别
// 模型目录需包含：det.onnx / rec.onnx / ppocr_keys.txt / onnxruntime.dll
// 失败（模型缺失、DLL 加载失败等）返回空字符串，由调用方回退到 PowerShell 方案。

#include <windows.h>
#include <string>
#include <vector>

namespace ocr_paddle {

// 单个聚合后的自然段 / 独立短语块（原图像素坐标 0..w, 0..h）
struct OcrBlock {
    RECT rc{};                   // 块整体外接矩形
    std::vector<RECT> lineRcs;   // 块内每一行的精确外接框（用于精准擦除原字，不误伤行尾空白处图案）
    float avgLineH = 0.0f;       // 原检测行框平均高度
    int lineCount = 0;           // 原始行数
    std::wstring text;           // 块内合并后的完整文本
};

// 初始化（幂等）。modelDir 末尾可带反斜杠。
bool Init(const std::wstring& modelDir);

// 自动按 exe 目录 / 数据目录尝试初始化（线程安全、幂等）
bool EnsureReady();

// 是否已可用
bool Ready();

// 识别一个图片文件（PNG/JPG/BMP，GDI+ 解码）。返回 UTF-8 文本（内部转宽字符）。
// outMs 可空，返回耗时毫秒；outConf 可空，返回置信度。
std::wstring RecognizeFile(const std::wstring& imagePath, int* outMs = nullptr, double* outConf = nullptr);

// 识别内存里的 BGRA 像素（截图选区直接用，免落盘）
std::wstring RecognizeBgra(const unsigned char* bgra, int width, int height, int stride,
                           int* outMs = nullptr, double* outConf = nullptr);

// 识别内存里的 BGRA 像素并聚合成适合「图内原位翻译」的自然段/独立词块
bool RecognizeBlocksBgra(const unsigned char* bgra, int width, int height, int stride,
                         std::vector<OcrBlock>& outBlocks, int* outMs = nullptr);

// 识别图片文件并输出聚合后的自然段/词块（便于命令行自测验证）
bool RecognizeBlocksFile(const std::wstring& imagePath, std::vector<OcrBlock>& outBlocks,
                         int* outMs = nullptr);

// 释放会话/环境
void Shutdown();

}  // namespace ocr_paddle