#pragma once
#include <windows.h>
#include <string>
#include <vector>

// 翻译厂家类型
enum class TrKind {
    Zhipu,   // 智谱 AI（GLM-4-Flash 永久免费，OpenAI 兼容）
    Index,   // 哔哩哔哩 Index-Translate（免费公网 API，OpenAI 兼容）
    Google,  // Google 翻译免费端点
    OpenAI,  // 任意 OpenAI 兼容服务（含自建/其他厂商中转）
};

struct Provider {
    TrKind kind = TrKind::OpenAI;
    std::wstring name;           // 显示名
    std::wstring base;           // 接口地址（不含 /chat/completions）
    std::wstring model;          // 模型名
    std::wstring key;            // API Key（Index/Google 可留空）
    bool enabled = true;         // 截图翻译是否启用
    bool selectEnabled = false;  // 划词翻译是否启用（默认智谱开启）
};

struct Settings {
    Settings();

    std::wstring screenshot = L"F1";   // 全局截图热键
    std::wstring pin = L"F2";          // 贴图
    std::wstring copyText = L"F3";     // 复制文字（OCR）
    std::wstring translate = L"F4";    // 翻译
    std::wstring copyImage = L"F5";    // 复制图片
    bool copyAfterShot = true;

    std::wstring saveDir;              // 截图保存主目录（空表示默认 图片\Zpix）
    bool autoSaveShot = true;          // 每次截图完成后自动留存到本地目录
    bool openFolderAfterSave = true;   // 点击下载/保存完成后自动打开对应文件夹
    int pickerFormat = 0;              // 取色格式：0=HEX 1=HEXA 2=RGB 3=RGBA 4=HSL 5=HSLA

    int imageSearchEngine = 0;         // 默认识图厂家：0=Yandex 1=Google 2=Bing 3=百度 4=SauceNAO 5=Trace.moe
    std::wstring imgbbKey;             // ImgBB 直链 API Key（标准版由构造函数注入，Pure 版为空）

    bool enableSelectTranslate = true;   // 全局划词翻译开关
    std::wstring targetLang = L"zh-CN";  // 外语默认译为（默认简体中文）
    std::wstring cnTargetLang = L"en";   // 中文默认译为（默认英语，避免中译中）
    std::vector<Provider> providers;     // 多厂家：启用几个就翻几个
};

// 保证至少有一个截图翻译厂家 & 至少有一个划词翻译厂家处于启用状态（无则兜底智谱或首项）
void EnsureProviderDefaults(std::vector<Provider>& list);

extern Settings g_cfg;

extern HWND g_msgWnd;
void ApplyHotkeys();  // 重新注册全局热键

std::wstring ConfigPath();
void LoadSettings();
void SaveSettings();
void ResetSettingsToDefault();

// 默认厂家列表（智谱 AI + B站 Index-Translate + Google，默认全部开启）
std::vector<Provider> DefaultProviders();
// 新建一个指定类型的厂家（带默认地址/模型）
Provider MakeProvider(TrKind kind);
const wchar_t* ProviderKindId(TrKind k);
const wchar_t* ProviderKindLabel(TrKind k);
TrKind ProviderKindFromId(const std::wstring& id);
bool IsBuiltInProvider(TrKind k);
bool IsModelLockedProvider(TrKind k);
bool HasApiKeyField(TrKind k);

// "Ctrl+Alt+A" <-> VK + MOD_*
bool ParseHotkey(const std::wstring& text, UINT* mods, UINT* vk);
std::wstring FormatHotkey(UINT mods, UINT vk);
const wchar_t* KeyName(UINT vk);