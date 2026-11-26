#include "config.h"
#include "util.h"

#include <map>
#include <vector>

Settings g_cfg;

static std::wstring ConfigDir() {
    // 便携优先：exe 旁可写就把配置放旁边；只读安装目录（Program Files）自动退回
    // %LOCALAPPDATA%\Zpix，权限判定统一在 DataDir() 里做
    return DataDir();
}
std::wstring ConfigPath() {
    std::wstring d = ConfigDir();
    if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
    return d + L"config.ini";
}

// ---------------------------------------------------------------- providers

const wchar_t* ProviderKindId(TrKind k) {
    switch (k) {
    case TrKind::Zhipu: return L"zhipu";
    case TrKind::Index: return L"index";
    case TrKind::Google: return L"google";
    case TrKind::OpenAI: return L"openai";
    }
    return L"openai";
}

const wchar_t* ProviderKindLabel(TrKind k) {
    switch (k) {
    case TrKind::Zhipu: return L"智谱AI";
    case TrKind::Index: return L"BliBli";
    case TrKind::Google: return L"Google";
    case TrKind::OpenAI: return L"OpenAI 兼容";
    }
    return L"OpenAI 兼容";
}

bool IsBuiltInProvider(TrKind k) {
    return k == TrKind::Zhipu || k == TrKind::Index || k == TrKind::Google;
}

bool IsModelLockedProvider(TrKind k) {
    return k == TrKind::Zhipu || k == TrKind::Index;
}

bool HasApiKeyField(TrKind k) {
    return k != TrKind::Index;
}

TrKind ProviderKindFromId(const std::wstring& id) {
    if (_wcsicmp(id.c_str(), L"zhipu") == 0) return TrKind::Zhipu;
    if (_wcsicmp(id.c_str(), L"index") == 0) return TrKind::Index;
    if (_wcsicmp(id.c_str(), L"google") == 0) return TrKind::Google;
    return TrKind::OpenAI;
}

// 默认不内置任何私有 API Key，由用户在设置页自行填入
Settings::Settings() : imgbbKey(L"") {}

Provider MakeProvider(TrKind kind) {
    Provider p;
    p.kind = kind;
    p.enabled = true;
    p.selectEnabled = false;
    switch (kind) {
    case TrKind::Zhipu:
        p.name = L"智谱AI";
        p.base = L"https://open.bigmodel.cn/api/paas/v4";
        p.model = L"glm-4-flash";
        // 默认无内置 Key，初始保持关闭状态；填入 Key 后自动开启
        p.enabled = false;
        p.selectEnabled = false;
        break;
    case TrKind::Index:
        p.name = L"BliBli";
        p.base = L"https://index-translate.bilibili.com/v1";
        p.model = L"Index-Translate-35B-A3B";
        p.enabled = true;
        p.selectEnabled = true;
        break;
    case TrKind::Google:
        p.name = L"Google 翻译";
        p.base = L"https://translate.googleapis.com";
        p.enabled = true;
        p.selectEnabled = false;
        break;
    default:
        p.name = L"自定义 OpenAI 服务";
        p.base = L"https://api.openai.com/v1";
        p.model = L"gpt-4o-mini";
        break;
    }
    return p;
}

void EnsureProviderDefaults(std::vector<Provider>& list) {
    // 默认三个内置厂家（智谱AI / BliBli / Google）禁止删除，缺失时按默认顺序补齐
    auto findKind = [&](TrKind k) -> int {
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].kind == k) return (int)i;
        return -1;
    };
    if (findKind(TrKind::Zhipu) < 0) list.insert(list.begin(), MakeProvider(TrKind::Zhipu));
    if (findKind(TrKind::Index) < 0) {
        int z = findKind(TrKind::Zhipu);
        list.insert(list.begin() + (z >= 0 ? z + 1 : 0), MakeProvider(TrKind::Index));
    }
    if (findKind(TrKind::Google) < 0) {
        int idx = findKind(TrKind::Index);
        list.insert(list.begin() + (idx >= 0 ? idx + 1 : (int)list.size()),
                    MakeProvider(TrKind::Google));
    }

    // 锁定智谱AI / BliBli 的固定模型；智谱未填 Key 时保持关闭状态
    for (auto& p : list) {
        if (p.kind == TrKind::Zhipu) {
            p.model = L"glm-4-flash";
            if (p.key.empty()) {
                p.enabled = false;
                p.selectEnabled = false;
            }
        } else if (p.kind == TrKind::Index) {
            p.model = L"Index-Translate-35B-A3B";
            p.key.clear();
        }
    }

    bool anyEnabled = false;
    bool anySelect = false;
    int indexIdx = findKind(TrKind::Index);
    int googleIdx = findKind(TrKind::Google);
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].enabled) anyEnabled = true;
        if (list[i].selectEnabled) anySelect = true;
    }
    if (!anyEnabled && !list.empty()) {
        if (indexIdx >= 0) list[indexIdx].enabled = true;
        if (googleIdx >= 0) list[googleIdx].enabled = true;
        if (indexIdx < 0 && googleIdx < 0) list[0].enabled = true;
    }
    if (!anySelect && !list.empty()) {
        int fb = (indexIdx >= 0) ? indexIdx : (googleIdx >= 0 ? googleIdx : 0);
        list[fb].selectEnabled = true;
    }
}

std::vector<Provider> DefaultProviders() {
    std::vector<Provider> v;
    v.push_back(MakeProvider(TrKind::Zhipu));
    v.push_back(MakeProvider(TrKind::Index));
    v.push_back(MakeProvider(TrKind::Google));
    return v;
}

// ---------------------------------------------------------------- key names

struct KeyEntry {
    const wchar_t* name;
    UINT vk;
};

static const KeyEntry kKeys[] = {
    {L"F1", VK_F1},   {L"F2", VK_F2},   {L"F3", VK_F3},   {L"F4", VK_F4},
    {L"F5", VK_F5},   {L"F6", VK_F6},   {L"F7", VK_F7},   {L"F8", VK_F8},
    {L"F9", VK_F9},   {L"F10", VK_F10}, {L"F11", VK_F11}, {L"F12", VK_F12},
    {L"Enter", VK_RETURN}, {L"Space", VK_SPACE}, {L"Tab", VK_TAB}, {L"Esc", VK_ESCAPE},
    {L"PrintScreen", VK_SNAPSHOT}, {L"Insert", VK_INSERT}, {L"Delete", VK_DELETE},
    {L"Home", VK_HOME}, {L"End", VK_END}, {L"Prior", VK_PRIOR}, {L"Next", VK_NEXT},
};

const wchar_t* KeyName(UINT vk) {
    static wchar_t buf[8];
    for (const auto& k : kKeys)
        if (k.vk == vk) return k.name;
    if (vk >= 'A' && vk <= 'Z') {
        buf[0] = (wchar_t)vk;
        buf[1] = 0;
        return buf;
    }
    if (vk >= '0' && vk <= '9') {
        buf[0] = (wchar_t)vk;
        buf[1] = 0;
        return buf;
    }
    buf[0] = 0;
    return buf;
}

static UINT KeyFromName(const std::wstring& name) {
    for (const auto& k : kKeys)
        if (_wcsicmp(k.name, name.c_str()) == 0) return k.vk;
    if (name.size() == 1) {
        wchar_t c = towupper(name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (UINT)c;
    }
    return 0;
}

bool ParseHotkey(const std::wstring& text, UINT* mods, UINT* vk) {
    if (!mods || !vk) return false;
    UINT m = 0, v = 0;
    size_t start = 0;
    std::wstring s = text;
    while (start <= s.size()) {
        size_t pos = s.find(L'+', start);
        std::wstring part = s.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
        // 去掉空格
        while (!part.empty() && (part.front() == L' ' || part.front() == L'\t')) part.erase(part.begin());
        while (!part.empty() && (part.back() == L' ' || part.back() == L'\t')) part.pop_back();

        if (!part.empty()) {
            if (_wcsicmp(part.c_str(), L"Ctrl") == 0 || _wcsicmp(part.c_str(), L"Control") == 0) m |= MOD_CONTROL;
            else if (_wcsicmp(part.c_str(), L"Alt") == 0) m |= MOD_ALT;
            else if (_wcsicmp(part.c_str(), L"Shift") == 0) m |= MOD_SHIFT;
            else if (_wcsicmp(part.c_str(), L"Win") == 0) m |= MOD_WIN;
            else v = KeyFromName(part);
        }
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    *mods = m;
    *vk = v;
    return v != 0;
}

std::wstring FormatHotkey(UINT mods, UINT vk) {
    std::wstring out;
    if (mods & MOD_CONTROL) out += L"Ctrl+";
    if (mods & MOD_ALT) out += L"Alt+";
    if (mods & MOD_SHIFT) out += L"Shift+";
    if (mods & MOD_WIN) out += L"Win+";
    const wchar_t* n = KeyName(vk);
    if (n && *n) out += n;
    return out;
}

// ---------------------------------------------------------------- ini io

static void Trim(std::wstring& s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
}

struct IniFile {
    std::vector<std::pair<std::wstring, std::wstring>> global;
    std::map<std::wstring, std::vector<std::pair<std::wstring, std::wstring>>> sections;

    std::wstring Get(const wchar_t* key) const {
        for (const auto& p : global)
            if (_wcsicmp(p.first.c_str(), key) == 0) return p.second;
        return L"";
    }
    std::wstring GetSec(const std::wstring& sec, const wchar_t* key) const {
        auto it = sections.find(sec);
        if (it == sections.end()) return L"";
        for (const auto& p : it->second)
            if (_wcsicmp(p.first.c_str(), key) == 0) return p.second;
        return L"";
    }
    // 扁平键：先找全局区，再找所有 [section]。
    // 旧版只查全局区，而 SaveSettings 把 screenshot / copyAfterShot / count …
    // 都写在 [hotkeys] / [general] / [translate] 里，导致重启后自定义厂家和快捷键全部丢失。
    std::wstring GetAny(const wchar_t* key) const {
        std::wstring v = Get(key);
        if (!v.empty()) return v;
        for (const auto& sec : sections)
            for (const auto& p : sec.second)
                if (_wcsicmp(p.first.c_str(), key) == 0) return p.second;
        return L"";
    }
};

static IniFile ParseIni(const std::wstring& text) {
    IniFile ini;
    std::wstring section;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring line = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        Trim(line);
        if (!line.empty() && line[0] == L'[') {
            size_t rb = line.find(L']');
            if (rb != std::wstring::npos) {
                section = line.substr(1, rb - 1);
                Trim(section);
            }
        } else if (!line.empty() && line[0] != L';' && line[0] != L'#') {
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos) {
                std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
                Trim(k);
                Trim(v);
                if (section.empty())
                    ini.global.emplace_back(k, v);
                else
                    ini.sections[section].emplace_back(k, v);
            }
        }
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    return ini;
}

void ResetSettingsToDefault() {
    g_cfg = Settings{};
    g_cfg.providers = DefaultProviders();
}

void LoadSettings() {
    g_cfg = Settings{};
    g_cfg.providers = DefaultProviders();
    g_cfg.targetLang = L"zh-CN";
    g_cfg.cnTargetLang = L"en";

    std::wstring path = ConfigPath();
    ZLog(L"[cfg] load from %ls", path.c_str());

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        SaveSettings();
        return;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string data((size_t)sz.QuadPart + 1, '\0');
    DWORD read = 0;
    ReadFile(h, data.data(), (DWORD)sz.QuadPart, &read, NULL);
    CloseHandle(h);
    data.resize(read);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), NULL, 0);
    std::wstring text(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data.c_str(), (int)data.size(), &text[0], wlen);

    IniFile ini = ParseIni(text);

    std::wstring v;
    if (!(v = ini.GetAny(L"screenshot")).empty()) g_cfg.screenshot = v;
    if (!(v = ini.GetAny(L"copyImage")).empty()) g_cfg.copyImage = v;
    if (!(v = ini.GetAny(L"pin")).empty()) g_cfg.pin = v;
    if (!(v = ini.GetAny(L"copyText")).empty()) g_cfg.copyText = v;
    if (!(v = ini.GetAny(L"translate")).empty()) g_cfg.translate = v;

    // 旧默认热键方案（Ctrl+Alt+A + F1~F4）一次性升级为新默认方案（F1 截图 / F2 贴图 / F3 文字 / F4 翻译 / F5 复制）
    bool hotkeyV2Migrated = (ini.GetAny(L"hotkeyV2Migrated") == L"1");
    bool hotkeyMigratedNow = false;
    if (!hotkeyV2Migrated) {
        if (_wcsicmp(g_cfg.screenshot.c_str(), L"Ctrl+Alt+A") == 0 &&
            _wcsicmp(g_cfg.copyImage.c_str(), L"F1") == 0) {
            g_cfg.screenshot = L"F1";
            g_cfg.pin = L"F2";
            g_cfg.copyText = L"F3";
            g_cfg.translate = L"F4";
            g_cfg.copyImage = L"F5";
        }
        hotkeyMigratedNow = true;
    }
    if (!(v = ini.GetAny(L"copyAfterShot")).empty()) g_cfg.copyAfterShot = (v == L"1");
    if (!(v = ini.GetAny(L"saveDir")).empty()) g_cfg.saveDir = NormalizeDirPath(v);
    if (!(v = ini.GetAny(L"autoSaveShot")).empty()) g_cfg.autoSaveShot = (v != L"0");
    if (!(v = ini.GetAny(L"openFolderAfterSave")).empty()) g_cfg.openFolderAfterSave = (v != L"0");
    if (!(v = ini.GetAny(L"pickerFormat")).empty()) {
        int pf = _wtoi(v.c_str());
        if (pf >= 0 && pf <= 5) g_cfg.pickerFormat = pf;
    }
    if (!(v = ini.GetSec(L"imgsearch", L"engine")).empty()) {
        int eng = _wtoi(v.c_str());
        if (eng >= 0 && eng <= 5) g_cfg.imageSearchEngine = eng;
    }
    {
        auto secIt = ini.sections.find(L"imgsearch");
        if (secIt != ini.sections.end()) {
            for (const auto& kv : secIt->second) {
                if (_wcsicmp(kv.first.c_str(), L"imgbbKey") == 0) {
                    g_cfg.imgbbKey = kv.second;
                    break;
                }
            }
        }
    }
    if (!(v = ini.GetAny(L"enableSelectTranslate")).empty())
        g_cfg.enableSelectTranslate = (v != L"0");
    if (!(v = ini.GetAny(L"targetLang")).empty()) g_cfg.targetLang = v;
    if (!(v = ini.GetAny(L"cnTargetLang")).empty()) g_cfg.cnTargetLang = v;

    // 厂家列表：count=N + [provider.i]
    int count = _wtoi(ini.GetAny(L"count").c_str());
    bool hadExplicitSelect = false;
    bool needSaveMigrated = false;
    std::vector<Provider> list;
    for (int i = 0; i < count; ++i) {
        wchar_t sec[32];
        swprintf(sec, 32, L"provider.%d", i);
        std::wstring kindId = ini.GetSec(sec, L"kind");
        if (kindId.empty()) continue;
        Provider p;
        p.kind = ProviderKindFromId(kindId);
        p.name = ini.GetSec(sec, L"name");
        p.base = ini.GetSec(sec, L"base");
        p.model = ini.GetSec(sec, L"model");
        p.key = ini.GetSec(sec, L"key");
        p.enabled = ini.GetSec(sec, L"enabled") != L"0";
        std::wstring selVal = ini.GetSec(sec, L"selectEnabled");
        if (selVal.empty()) {
            p.selectEnabled = (p.kind == TrKind::Index || (p.kind == TrKind::Zhipu && !p.key.empty()));
        } else {
            p.selectEnabled = (selVal != L"0");
            hadExplicitSelect = true;
        }
        // 自动升级旧默认显示名 & 锁定固定模型
        if (p.kind == TrKind::Zhipu) {
            if (p.name.empty() || p.name == L"智谱 AI（GLM-4-Flash）" || p.name == L"智谱 AI") {
                if (p.name != L"智谱AI") needSaveMigrated = true;
                p.name = L"智谱AI";
            }
            if (p.model != L"glm-4-flash") {
                p.model = L"glm-4-flash";
                needSaveMigrated = true;
            }
        } else if (p.kind == TrKind::Index) {
            if (p.name.empty() || p.name == L"Index-Translate（B站免费）" ||
                p.name == L"Index-Translate") {
                if (p.name != L"BliBli") needSaveMigrated = true;
                p.name = L"BliBli";
            }
            if (p.model != L"Index-Translate-35B-A3B") {
                p.model = L"Index-Translate-35B-A3B";
                needSaveMigrated = true;
            }
            if (!p.key.empty()) {
                p.key.clear();
                needSaveMigrated = true;
            }
        }
        if (p.name.empty()) p.name = ProviderKindLabel(p.kind);
        list.push_back(p);
    }

    if (!list.empty()) {
        size_t beforeSz = list.size();
        bool hadZhipu = false;
        for (const auto& p : list)
            if (p.kind == TrKind::Zhipu) { hadZhipu = true; break; }
        if (!hadZhipu && !hadExplicitSelect) {
            for (auto& p : list) p.selectEnabled = false;
        }
        EnsureProviderDefaults(list);
        if (list.size() != beforeSz) needSaveMigrated = true;
        g_cfg.providers = list;
        if (needSaveMigrated || hotkeyMigratedNow) SaveSettings();
    } else if (hotkeyMigratedNow) {
        SaveSettings();
    }
}

void SaveSettings() {
    EnsureProviderDefaults(g_cfg.providers);
    std::wstring text;
    text += L"; Zpix config\r\n";
    text += L"[hotkeys]\r\n";
    text += L"hotkeyV2Migrated=1\r\n";
    text += L"screenshot=" + g_cfg.screenshot + L"\r\n";
    text += L"pin=" + g_cfg.pin + L"\r\n";
    text += L"copyText=" + g_cfg.copyText + L"\r\n";
    text += L"translate=" + g_cfg.translate + L"\r\n";
    text += L"copyImage=" + g_cfg.copyImage + L"\r\n";
    text += L"\r\n[general]\r\n";
    text += std::wstring(L"copyAfterShot=") + (g_cfg.copyAfterShot ? L"1" : L"0") + L"\r\n";
    text += L"pickerFormat=" + std::to_wstring(g_cfg.pickerFormat) + L"\r\n";
    text += L"\r\n[save]\r\n";
    text += L"saveDir=" + NormalizeDirPath(g_cfg.saveDir) + L"\r\n";
    text += std::wstring(L"autoSaveShot=") + (g_cfg.autoSaveShot ? L"1" : L"0") + L"\r\n";
    text += std::wstring(L"openFolderAfterSave=") + (g_cfg.openFolderAfterSave ? L"1" : L"0") + L"\r\n";
    text += L"\r\n[imgsearch]\r\n";
    text += L"engine=" + std::to_wstring(g_cfg.imageSearchEngine) + L"\r\n";
    text += L"imgbbKey=" + g_cfg.imgbbKey + L"\r\n";
    text += L"\r\n[translate]\r\n";
    text += std::wstring(L"enableSelectTranslate=") + (g_cfg.enableSelectTranslate ? L"1" : L"0") +
            L"\r\n";
    text += L"zhipuMigrated=1\r\n";
    text += L"targetLang=" + g_cfg.targetLang + L"\r\n";
    text += L"cnTargetLang=" + g_cfg.cnTargetLang + L"\r\n";
    text += L"count=" + std::to_wstring(g_cfg.providers.size()) + L"\r\n";
    for (size_t i = 0; i < g_cfg.providers.size(); ++i) {
        const Provider& p = g_cfg.providers[i];
        text += L"\r\n[provider." + std::to_wstring(i) + L"]\r\n";
        text += std::wstring(L"kind=") + ProviderKindId(p.kind) + L"\r\n";
        text += L"name=" + p.name + L"\r\n";
        text += L"base=" + p.base + L"\r\n";
        text += L"model=" + p.model + L"\r\n";
        text += L"key=" + p.key + L"\r\n";
        text += std::wstring(L"enabled=") + (p.enabled ? L"1" : L"0") + L"\r\n";
        text += std::wstring(L"selectEnabled=") + (p.selectEnabled ? L"1" : L"0") + L"\r\n";
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8(len > 0 ? len - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8.data(), len, NULL, NULL);

    std::wstring path = ConfigPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ZLog(L"[cfg] save FAILED err=%lu path=%ls", GetLastError(), path.c_str());
        return;
    }
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, NULL);
    CloseHandle(h);
    ZLog(L"[cfg] saved to %ls (providers=%d)", path.c_str(), (int)g_cfg.providers.size());
}