#pragma once
#include "translate.h"
#include "util.h"

#include <string>
#include <vector>

enum class ShotAction { CopyImage, Pin, Save, CopyText, Translate, ImageSearch };

// 翻译、OCR 与以图搜图都在后台线程完成，用消息把结果送回 UI 线程
const UINT WM_ZPIX_TRANSLATE_DONE = WM_APP + 20;
const UINT WM_ZPIX_OCR_DONE = WM_APP + 22;
const UINT WM_ZPIX_IMGSEARCH_DONE = WM_APP + 28;

struct ImgSearchDonePayload {
    bool ok = false;
    std::wstring engineName;
    std::wstring targetUrl;
    std::wstring error;
};

void HandleImgSearchDone(ImgSearchDonePayload* payload);

struct TranslatePayload {
    std::wstring original;
    std::vector<TrResult> results;  // 每个已启用厂家一条
    HWND target = NULL;             // 已打开的结果窗口（可能为 0）
    uint64_t reqId = 0;             // 请求序号（窗口内切换目标语言时防旧回包覆盖）
};

struct OcrPayload {
    std::wstring text;
    bool translate = false;
};

// 统一执行：复制 / 贴图 / 保存 / OCR 取字 / 翻译
bool DoShotAction(ShotAction action, const Bitmap32& img, HWND owner);

// 后台 OCR 完成后回到 UI 线程处理（复制文字或发起翻译）
void HandleOcrDone(OcrPayload* payload);

// OCR：把 png 交给系统 OCR（Windows.Media.Ocr，通过 PowerShell 脚本调用）
std::wstring OcrImageFile(const std::wstring& pngPath);

// 记录本次操作的屏幕选区中心：多屏时用决定结果窗口落在哪块屏、按哪块屏的 DPI 排版
void SetResultAnchor(RECT selScreen);

// 结果窗口（翻译用）：先打开占位，后台翻译完再回填多厂家结果
HWND ShowResultWindow(const std::wstring& original, bool selectMode = false);

// 当前结果窗口的活动请求序号
uint64_t CurrentResultReqId();

// 后台翻译完成后填充结果
void SetResultPayload(HWND hwnd, const std::wstring& original, const std::vector<TrResult>& results,
                      uint64_t reqId = 0);

// exe 所在目录（带尾部反斜杠）
std::wstring ExeDirOf();

// 写 UTF-8 文本文件
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text);

// 复制纯文本到剪贴板
bool CopyTextToClipboard(HWND owner, const std::wstring& text);

// 轻提示（屏幕下方浮一个 1.5 秒的小窗）
void ShowToast(const std::wstring& text);