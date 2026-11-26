#pragma once
#include "config.h"

#include <string>
#include <vector>

// 单个厂家的翻译结果
struct TrResult {
    std::wstring provider;  // 显示名
    std::wstring tag;       // 类型标签
    std::wstring text;      // 译文（失败为空）
    std::wstring error;     // 失败原因
    int ms = 0;             // 用时（毫秒）
};

// 可选目标语言项
struct LangOption {
    const wchar_t* code;   // 如 zh-CN / en / ja / de ...
    const wchar_t* label;  // UI 显示名（如 简体中文 / 英语）
};

const std::vector<LangOption>& SupportedTargetLangs();
std::wstring LangDisplayName(const std::wstring& code);

// 目标语言代码 → 提示词里的语言名（zh-CN → 简体中文）
std::wstring LangNameForPrompt(const std::wstring& code);

// 识别原文主要语种展示名（如 中文 / English / 日语 / 德语·拉丁 / 韩语 / 俄语）
std::wstring DetectSourceLangLabel(const std::wstring& text);

// 判断原文是否主要为中文（自动排除含假名的日文）
bool IsMostlyChineseText(const std::wstring& text);

// 解析本次实际使用的目标语言：overrideLang 非空时优先，否则按「中文→cnTargetLang / 外语→targetLang」自动决定
std::wstring ResolveTargetLang(const std::wstring& text, const std::wstring& overrideLang = L"");

// 跑所有已启用的截图翻译厂家（并行），返回按配置顺序排列的结果
std::vector<TrResult> TranslateAll(const std::wstring& text,
                                   const std::wstring& overrideLang = L"");

// 跑所有已启用「划词翻译」的厂家（并行，无则默认兜底智谱）
std::vector<TrResult> TranslateSelectAll(const std::wstring& text,
                                         const std::wstring& overrideLang = L"");

// 单个厂家（失败时 err 非空；targetLang 为空则按原文自动判定）
std::wstring TranslateWithProvider(const Provider& p, const std::wstring& text, std::wstring* err,
                                   const std::wstring& targetLang = L"");

// 批量一对一翻译多个段落/词块（供图内原位翻译使用：大模型走单次带序号请求，漏项或 Google 走并发补齐）
bool TranslateBatch(const std::vector<std::wstring>& texts,
                    std::vector<std::wstring>& outTranslated,
                    const std::wstring& targetLang,
                    std::wstring* outProvider = nullptr,
                    std::wstring* err = nullptr);

// 基础 HTTP（支持 GET / POST，https 自动走 TLS）
bool HttpRequest(const std::wstring& verb, const std::wstring& url, const std::wstring& headers,
                 const std::string& body, std::string& out, DWORD* err);