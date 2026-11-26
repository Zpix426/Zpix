#include "settings.h"
#include "actions.h"
#include "config.h"
#include "img_search.h"
#include "select_translate.h"
#include "ui.h"
#include "util.h"

#include <shellapi.h>
#include <cmath>
#include <string>
#include <vector>

namespace {

static const wchar_t* kMaskedKey = L"**********";
static const wchar_t* kZhipuKeyUrl = L"https://bigmodel.cn/apikey/platform";
static const wchar_t* kImgbbKeyUrl = L"https://api.imgbb.com/";
const UINT_PTR kTimerAnim = 1;
const UINT_PTR kTimerAutoSave = 2;

// ---------------------------------------------------------------- 尺寸基线（96dpi）
const int kBaseW = 660;
const int kTitleH = 52;
const int kPad = 24;
const int kRowH = 52;
const int kHotCount = 5;
const int kFooterH = 64;
const int kProvH = 176;
const int kTabH = 40;
const int kTabW = 110;
const int kTabCount = 4;

const wchar_t* kHotLabels[kHotCount] = {L"截图（全局热键）", L"贴图", L"复制文字（OCR）", L"翻译",
                                        L"复制图片"};
enum { TAB_HOTKEY = 0, TAB_TRANSLATE = 1, TAB_IMGSEARCH = 2, TAB_SAVE = 3 };
const wchar_t* kTabLabels[kTabCount] = {L"快捷键设置", L"翻译服务", L"以图搜图", L"保存设置"};

// 控件命中 id
enum {
    ID_NONE = 0,
    ID_CLOSE = 1,
    ID_HOTKEY0 = 10,          // +0..4
    ID_PICK_DIR = 30,
    ID_OPEN_DIR = 31,
    ID_TOGGLE_AUTOSAVE = 32,
    ID_TOGGLE_OPENFOLDER = 33,
    ID_TOGGLE_SELECT_MASTER = 34,
    ID_LANG_FOREIGN = 35,
    ID_LANG_CHINESE = 36,
    ID_GETKEY_IMGBB = 37,
    ID_EDIT_SAVEDIR = 550,
    ID_EDIT_IMGBB_KEY = 560,
    ID_IMGENGINE0 = 570,      // +0..5
    ID_TAB0 = 50,             // +0..3
    ID_TOGGLE0 = 100,         // +0..n
    ID_DEL0 = 200,            // +0..n
    ID_SELECT0 = 300,         // +0..n（划词翻译启用按钮）
    ID_GETKEY0 = 400,         // +0..n（免费获取 API Key 链接）
    ID_ADD = 40,
    ID_RESET = 41,
    ID_CANCEL = 42,
    ID_SAVE = 43,
    ID_SCROLLBAR = 44,
};

struct SettingsState {
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    bool iconOwned = false;
    int scale = 100;
    int width = 660;
    int height = 760;
    int contentTop = 0;
    int contentH = 0;
    int scroll = 0;
    int scrollMax = 0;
    bool scrollDrag = false;
    int scrollDragStartY = 0;
    int scrollDragStartVal = 0;
    RECT rcScrollTrack{};
    RECT rcScrollThumb{};
    int tab = 0;

    int capture = -1;
    int hover = ID_NONE;
    int pressed = ID_NONE;
    bool tracking = false;
    bool winDragging = false;
    bool inSizing = false;
    double dragRx = 0.5;
    double dragRy = 0.03;
    int dragOffX = 0;
    int dragOffY = 0;

    RECT rcClose{};
    RECT rcTabBar{};
    RECT rcTab[kTabCount]{};

    // Tab: 保存设置
    RECT rcSaveDirBox{};
    RECT rcPickDir{};
    RECT rcOpenDir{};
    RECT rcToggleAutoSave{};
    RECT rcToggleOpenFolder{};
    RECT rcSavePreview{};
    HWND hSaveDir = nullptr;

    // Tab: 以图搜图
    RECT rcImgEngineCard{};
    RECT rcImgEngines[IMG_ENGINE_COUNT]{};
    RECT rcImgbbCard{};
    RECT rcImgbbKey{};
    RECT rcImgbbGetKey{};
    RECT rcImgHint{};
    HWND hImgbbKey = nullptr;
    bool imgbbKeyEdited = false;

    // Tab 1: 快捷键
    RECT rcHot[kHotCount]{};
    RECT rcHint{};

    // Tab 2: 翻译服务
    RECT rcSelectMaster{};
    RECT rcLangCard{}, rcLangForeign{}, rcLangChinese{};
    RECT rcAdd{};
    RECT rcReset{}, rcCancel{}, rcSave{};
    std::vector<RECT> rcCard, rcToggle, rcSelect, rcDel, rcBase, rcModel, rcKey, rcGetKey;
    std::vector<HWND> hBase, hModel, hKey;
    std::vector<bool> keyEdited;
    bool suppressKeyEvent = false;
    bool savePending = false;

    HFONT fTitle = nullptr, fSub = nullptr, fSection = nullptr, fLabel = nullptr, fSmall = nullptr,
          fField = nullptr, fBtn = nullptr, fTab = nullptr;
    HBRUSH brushField = nullptr;

    float aSave = 0, aCancel = 0, aReset = 0, aAdd = 0, aClose = 0, aPickDir = 0, aOpenDir = 0;
    float aAutoSave = 1.0f, aOpenFolder = 1.0f, aSelectMaster = 1.0f;
    float aTabHover[kTabCount]{};
    std::vector<float> aToggle;
};

SettingsState* g_st = nullptr;
int g_pendingInitialTab = -1;

int S(int v) { return g_st ? MulDiv(v, g_st->scale, 100) : v; }

// 按当前 scale 重建全部字体；子输入框跟着换，避免字号和窗口尺寸脱节
void RebuildFonts() {
    if (!g_st) return;
    HFONT* fs[] = {&g_st->fTitle, &g_st->fSub, &g_st->fSection, &g_st->fLabel,
                   &g_st->fSmall, &g_st->fField, &g_st->fBtn,     &g_st->fTab};
    for (HFONT* f : fs)
        if (*f) {
            DeleteObject(*f);
            *f = nullptr;
        }
    auto mk = [&](int px, bool bold) { return MakeUiFont(MulDiv(px, g_st->scale, 100), bold); };
    g_st->fTitle = mk(15, true);
    g_st->fSub = mk(13, false);
    g_st->fSection = mk(15, true);
    g_st->fLabel = mk(13, false);
    g_st->fSmall = mk(12, false);
    g_st->fField = mk(13, false);
    g_st->fBtn = mk(13, true);
    g_st->fTab = mk(13, true);

    auto refontOne = [&](HWND h) {
        if (!h) return;
        SendMessageW(h, WM_SETFONT, (WPARAM)g_st->fField, TRUE);
        SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(S(2), S(2)));
    };
    auto refont = [&](std::vector<HWND>& v) {
        for (HWND h : v) refontOne(h);
    };
    refont(g_st->hBase);
    refont(g_st->hModel);
    refont(g_st->hKey);
    refontOne(g_st->hSaveDir);
    refontOne(g_st->hImgbbKey);
}

// 窗口重新定位的三种场合
enum {
    PlaceCenterPrimary = 0,  // 首次打开：主屏工作区正中
    PlaceKeepTopLeft = 1,    // 切 tab / 重置：位置不动
    PlaceKeepCenter = 2,     // DPI 变了：中心点不动，尺寸重算
};

std::wstring* HotPtr(int i) {
    switch (i) {
    case 0: return &g_cfg.screenshot;
    case 1: return &g_cfg.pin;
    case 2: return &g_cfg.copyText;
    case 3: return &g_cfg.translate;
    case 4: return &g_cfg.copyImage;
    }
    return nullptr;
}

bool PtIn(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// ---------------------------------------------------------------- 子输入框

void DestroyFieldEdits() {
    if (!g_st) return;
    if (g_st->hSaveDir) {
        DestroyWindow(g_st->hSaveDir);
        g_st->hSaveDir = nullptr;
    }
    if (g_st->hImgbbKey) {
        DestroyWindow(g_st->hImgbbKey);
        g_st->hImgbbKey = nullptr;
    }
    for (HWND h : g_st->hBase)
        if (h) DestroyWindow(h);
    for (HWND h : g_st->hModel)
        if (h) DestroyWindow(h);
    for (HWND h : g_st->hKey)
        if (h) DestroyWindow(h);
    g_st->hBase.clear();
    g_st->hModel.clear();
    g_st->hKey.clear();
}

HWND MakeField(const std::wstring& text, int id) {
    HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10,
                             g_st->hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(e, WM_SETFONT, (WPARAM)g_st->fField, TRUE);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(S(2), S(2)));
    SendMessageW(e, EM_SETLIMITTEXT, 2048, 0);
    return e;
}

void CreateFieldEdits() {
    if (!g_st) return;
    DestroyFieldEdits();
    g_st->suppressKeyEvent = true;
    std::wstring dispDir = g_cfg.saveDir.empty() ? EffectiveSaveRootDir(L"") : g_cfg.saveDir;
    g_st->hSaveDir = MakeField(dispDir, ID_EDIT_SAVEDIR);
    g_st->imgbbKeyEdited = false;
    g_st->hImgbbKey = MakeField(g_cfg.imgbbKey.empty() ? L"" : kMaskedKey, ID_EDIT_IMGBB_KEY);
    SendMessageW(g_st->hImgbbKey, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
    g_st->aAutoSave = g_cfg.autoSaveShot ? 1.0f : 0.0f;
    g_st->aOpenFolder = g_cfg.openFolderAfterSave ? 1.0f : 0.0f;
    g_st->aSelectMaster = g_cfg.enableSelectTranslate ? 1.0f : 0.0f;
    g_st->keyEdited.assign(g_cfg.providers.size(), false);
    for (size_t i = 0; i < g_cfg.providers.size(); ++i) {
        const Provider& p = g_cfg.providers[i];
        g_st->hBase.push_back(MakeField(p.base, 600 + (int)i * 10));
        g_st->hModel.push_back(IsModelLockedProvider(p.kind) ? NULL
                                                             : MakeField(p.model, 601 + (int)i * 10));
        if (HasApiKeyField(p.kind)) {
            // 已有 Key 默认用 ********** 隐藏，点击输入框后自动清空以便输入新 Key，输入时也以 * 掩码显示
            std::wstring masked = p.key.empty() ? L"" : kMaskedKey;
            HWND ek = MakeField(masked, 602 + (int)i * 10);
            SendMessageW(ek, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
            g_st->hKey.push_back(ek);
        } else {
            g_st->hKey.push_back(NULL);
        }
    }
    g_st->suppressKeyEvent = false;
    g_st->aToggle.assign(g_cfg.providers.size(), 0.0f);
    for (size_t i = 0; i < g_cfg.providers.size(); ++i)
        g_st->aToggle[i] = g_cfg.providers[i].enabled ? 1.0f : 0.0f;
}

static std::wstring TrimKeyText(std::wstring s) {
    while (!s.empty() &&
           (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n'))
        s.erase(s.begin());
    while (!s.empty() &&
           (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n'))
        s.pop_back();
    return s;
}

// 从子输入框回读文本到配置（resetMask=false 用于输入中实时同步，不打断光标）
void PullFieldText(bool resetMask = true) {
    if (!g_st) return;
    if (g_st->hSaveDir) {
        wchar_t buf[2048] = {};
        GetWindowTextW(g_st->hSaveDir, buf, 2048);
        g_cfg.saveDir = NormalizeDirPath(buf);
    }
    if (g_st->hImgbbKey && g_st->imgbbKeyEdited) {
        wchar_t buf[2048] = {};
        GetWindowTextW(g_st->hImgbbKey, buf, 2048);
        std::wstring val = TrimKeyText(buf);
        if (val != kMaskedKey) {
            g_cfg.imgbbKey = val;
        }
        if (resetMask) {
            g_st->imgbbKeyEdited = false;
            g_st->suppressKeyEvent = true;
            SetWindowTextW(g_st->hImgbbKey, g_cfg.imgbbKey.empty() ? L"" : kMaskedKey);
            g_st->suppressKeyEvent = false;
        }
    }
    for (size_t i = 0; i < g_cfg.providers.size() && i < g_st->hBase.size(); ++i) {
        wchar_t buf[2048] = {};
        if (g_st->hBase[i]) {
            GetWindowTextW(g_st->hBase[i], buf, 2048);
            g_cfg.providers[i].base = buf;
        }
        if (i < g_st->hModel.size() && g_st->hModel[i]) {
            GetWindowTextW(g_st->hModel[i], buf, 2048);
            g_cfg.providers[i].model = buf;
        }
        if (i < g_st->hKey.size() && g_st->hKey[i] &&
            i < g_st->keyEdited.size() && g_st->keyEdited[i]) {
            GetWindowTextW(g_st->hKey[i], buf, 2048);
            std::wstring val = TrimKeyText(buf);
            if (val != kMaskedKey) {
                bool wasEmpty = g_cfg.providers[i].key.empty();
                g_cfg.providers[i].key = val;
                if (g_cfg.providers[i].kind == TrKind::Zhipu) {
                    if (wasEmpty && !val.empty()) {
                        g_cfg.providers[i].enabled = true;
                    } else if (val.empty()) {
                        g_cfg.providers[i].enabled = false;
                        g_cfg.providers[i].selectEnabled = false;
                        EnsureProviderDefaults(g_cfg.providers);
                    }
                }
            }
            if (resetMask) {
                g_st->keyEdited[i] = false;
                g_st->suppressKeyEvent = true;
                SetWindowTextW(g_st->hKey[i], g_cfg.providers[i].key.empty() ? L"" : kMaskedKey);
                g_st->suppressKeyEvent = false;
            }
        }
    }
}

// 立即同步内存并持久化到 config.ini
void CommitRealtimeSave(bool resetMask = false) {
    if (g_st) {
        if (g_st->hwnd && g_st->savePending) {
            KillTimer(g_st->hwnd, kTimerAutoSave);
        }
        g_st->savePending = false;
        PullFieldText(resetMask);
    }
    SaveSettings();
}

void KickAnim();

// 输入框逐字修改时：0ms 同步内存配置（立即生效），250ms 防抖自动落盘
void ScheduleRealtimeSave() {
    if (!g_st) return;
    PullFieldText(false);
    g_st->savePending = true;
    if (g_st->hwnd) {
        SetTimer(g_st->hwnd, kTimerAutoSave, 250, NULL);
        KickAnim();
        InvalidateRect(g_st->hwnd, NULL, FALSE);
    }
}

// ---------------------------------------------------------------- 布局

void ComputeLayout(bool useActualHeight = false) {
    if (!g_st) return;
    const int pad = S(kPad);
    const int contentW = g_st->width - pad * 2;

    // 标题栏 + 副标题 + tab 栏（固定在顶部，不随内容滚动）
    int subY = S(kTitleH) + S(4);
    int tabY = subY + S(30);
    g_st->rcTabBar = RECT{pad, tabY, pad + S(kTabW) * kTabCount + S(8), tabY + S(kTabH)};
    int inner = S(4);
    int tabW = (g_st->rcTabBar.right - g_st->rcTabBar.left - inner * 2) / kTabCount;
    for (int t = 0; t < kTabCount; ++t) {
        int x0 = g_st->rcTabBar.left + inner + t * tabW;
        int x1 = (t == kTabCount - 1) ? (g_st->rcTabBar.right - inner) : (x0 + tabW);
        g_st->rcTab[t] = RECT{x0, tabY + inner, x1, tabY + S(kTabH) - inner};
    }

    const int contentTop = tabY + S(kTabH) + S(20);
    g_st->contentTop = contentTop;

    // 窗口高度统一按「快捷键设置」页高度固定；其他页内容超出时自动走滚动条
    const int hotkeyBottom =
        contentTop + S(48) + S(kRowH) * kHotCount + S(12) + S(14) + S(64) + S(16);
    int bodyBottom = hotkeyBottom;

    // 先计算未滚动的内容总高度
    if (g_st->tab == TAB_HOTKEY) {
        bodyBottom = hotkeyBottom;
    } else if (g_st->tab == TAB_TRANSLATE) {
        int secY = contentTop;
        int langCardH = S(52);
        int provY = secY + S(48) + langCardH + S(10);
        int provN = (int)g_cfg.providers.size();
        int addY = provY + provN * (S(kProvH) + S(10));
        bodyBottom = addY + S(42) + S(16);
    } else if (g_st->tab == TAB_IMGSEARCH) {
        int secY = contentTop;
        int engCardY = secY + S(48);
        int engCardH = S(132);
        int keyCardY = engCardY + engCardH + S(12);
        int keyCardH = S(116);
        int hintY = keyCardY + keyCardH + S(12);
        int hintH = S(64);
        bodyBottom = hintY + hintH + S(16);
    } else {
        int secY = contentTop;
        int dirCardY = secY + S(48);
        int dirCardH = S(104);
        int optCardY = dirCardY + dirCardH + S(12);
        int optCardH = S(124);
        int preY = optCardY + optCardH + S(12);
        int preH = S(64);
        bodyBottom = preY + preH + S(16);
    }

    g_st->contentH = bodyBottom;
    if (!useActualHeight) {
        g_st->height = hotkeyBottom + S(kFooterH);
        return;
    }

    int viewTop = contentTop;
    int viewBottom = g_st->height - S(kFooterH);
    int overflow = bodyBottom - viewBottom;
    g_st->scrollMax = overflow > 0 ? overflow : 0;
    if (g_st->scroll > g_st->scrollMax) g_st->scroll = g_st->scrollMax;
    if (g_st->scroll < 0) g_st->scroll = 0;

    g_st->rcScrollTrack = RECT{};
    g_st->rcScrollThumb = RECT{};
    if (g_st->scrollMax > 0) {
        int trackTop = contentTop + S(4);
        int trackH = viewBottom - contentTop - S(8);
        int totalH = g_st->contentH - contentTop;
        if (trackH > S(20) && totalH > 0) {
            int thumbH = MulDiv(trackH, trackH, totalH);
            if (thumbH < S(28)) thumbH = S(28);
            if (thumbH > trackH) thumbH = trackH;
            int ty = trackTop + MulDiv(trackH - thumbH, g_st->scroll, g_st->scrollMax);
            int bw = S(6);
            if (bw < 4) bw = 4;
            int bx = g_st->width - pad + S(7);
            g_st->rcScrollTrack = RECT{bx - S(4), trackTop, bx + bw + S(4), trackTop + trackH};
            g_st->rcScrollThumb = RECT{bx, ty, bx + bw, ty + thumbH};
        }
    }

    int off = -g_st->scroll;

    // 清空非当前 tab 的命中矩形
    g_st->rcSaveDirBox = RECT{};
    g_st->rcPickDir = RECT{};
    g_st->rcOpenDir = RECT{};
    g_st->rcToggleAutoSave = RECT{};
    g_st->rcToggleOpenFolder = RECT{};
    g_st->rcSavePreview = RECT{};
    g_st->rcImgEngineCard = RECT{};
    for (int i = 0; i < IMG_ENGINE_COUNT; ++i) g_st->rcImgEngines[i] = RECT{};
    g_st->rcImgbbCard = RECT{};
    g_st->rcImgbbKey = RECT{};
    g_st->rcImgbbGetKey = RECT{};
    g_st->rcImgHint = RECT{};
    g_st->rcHint = RECT{};
    g_st->rcSelectMaster = RECT{};
    g_st->rcLangCard = RECT{};
    g_st->rcLangForeign = RECT{};
    g_st->rcLangChinese = RECT{};
    g_st->rcAdd = RECT{};
    for (int i = 0; i < kHotCount; ++i) g_st->rcHot[i] = RECT{};
    g_st->rcCard.clear();
    g_st->rcToggle.clear();
    g_st->rcSelect.clear();
    g_st->rcDel.clear();
    g_st->rcBase.clear();
    g_st->rcModel.clear();
    g_st->rcKey.clear();
    g_st->rcGetKey.clear();

    if (g_st->tab == TAB_HOTKEY) {
        int secY = contentTop + off;
        int cardY = secY + S(48);
        int cardH = S(kRowH) * kHotCount + S(12);
        int hintY = cardY + cardH + S(14);
        int hintH = S(64);
        for (int i = 0; i < kHotCount; ++i) {
            int ry = cardY + S(6) + i * S(kRowH);
            int fx = pad + contentW - S(18) - S(140);
            g_st->rcHot[i] = RECT{fx, ry + (S(kRowH) - S(38)) / 2, fx + S(140),
                                  ry + (S(kRowH) - S(38)) / 2 + S(38)};
        }
        g_st->rcHint = RECT{pad, hintY, pad + contentW, hintY + hintH};
    } else if (g_st->tab == TAB_TRANSLATE) {
        int secY = contentTop + off;
        g_st->rcSelectMaster = RECT{pad + contentW - S(36), secY + S(2), pad + contentW, secY + S(22)};
        int langY = secY + S(48);
        int langH = S(52);
        g_st->rcLangCard = RECT{pad, langY, pad + contentW, langY + langH};
        int midX = g_st->rcLangCard.left + contentW / 2;
        int ddW = S(122), ddH = S(32);
        int ddY = langY + (langH - ddH) / 2;
        g_st->rcLangForeign = RECT{midX - S(16) - ddW, ddY, midX - S(16), ddY + ddH};
        g_st->rcLangChinese = RECT{g_st->rcLangCard.right - S(16) - ddW, ddY,
                                   g_st->rcLangCard.right - S(16), ddY + ddH};

        int provY = langY + langH + S(10);
        int provN = (int)g_cfg.providers.size();
        int addY = provY + provN * (S(kProvH) + S(10));

        g_st->rcCard.assign(provN, RECT{});
        g_st->rcToggle.assign(provN, RECT{});
        g_st->rcSelect.assign(provN, RECT{});
        g_st->rcDel.assign(provN, RECT{});
        g_st->rcBase.assign(provN, RECT{});
        g_st->rcModel.assign(provN, RECT{});
        g_st->rcKey.assign(provN, RECT{});
        g_st->rcGetKey.assign(provN, RECT{});

        HDC measDc = GetDC(g_st->hwnd);
        HGDIOBJ oldF = measDc ? SelectObject(measDc, g_st->fLabel) : nullptr;
        for (int i = 0; i < provN; ++i) {
            const Provider& p = g_cfg.providers[i];
            int cy = provY + i * (S(kProvH) + S(10));
            RECT card{pad, cy, pad + contentW, cy + S(kProvH)};
            g_st->rcCard[i] = card;
            g_st->rcToggle[i] = RECT{card.left + S(16), card.top + S(16), card.left + S(16) + S(36),
                                     card.top + S(16) + S(20)};
            if (!IsBuiltInProvider(p.kind)) {
                g_st->rcDel[i] = RECT{card.right - S(16) - S(52), card.top + S(12),
                                      card.right - S(16), card.top + S(12) + S(24)};
            } else {
                g_st->rcDel[i] = RECT{};
            }

            // 厂家名称右侧紧跟「划词翻译」独立开关按钮
            std::wstring dispName = p.name.empty() ? ProviderKindLabel(p.kind) : p.name;
            SIZE nsz{};
            if (measDc) GetTextExtentPoint32W(measDc, dispName.c_str(), (int)dispName.size(), &nsz);
            int nx = g_st->rcToggle[i].right + S(12);
            int pillW = S(84);
            int pillH = S(22);
            int pillX = nx + nsz.cx + S(10);
            int rightLimit = IsBuiltInProvider(p.kind) ? (card.right - S(16)) : (g_st->rcDel[i].left - S(10));
            int maxPillX = rightLimit - pillW;
            if (pillX > maxPillX) pillX = maxPillX;
            g_st->rcSelect[i] = RECT{pillX, card.top + S(9), pillX + pillW, card.top + S(9) + pillH};

            int fx = card.left + S(16);
            int fw = card.right - card.left - S(32);
            g_st->rcBase[i] = RECT{fx, card.top + S(72), fx + fw, card.top + S(72) + S(34)};
            if (HasApiKeyField(p.kind)) {
                int halfW = (fw - S(12)) / 2;
                g_st->rcModel[i] = RECT{fx, card.top + S(130), fx + halfW, card.top + S(130) + S(34)};
                g_st->rcKey[i] =
                    RECT{fx + halfW + S(12), card.top + S(130), fx + fw, card.top + S(130) + S(34)};
                if (p.kind == TrKind::Zhipu) {
                    g_st->rcGetKey[i] = RECT{g_st->rcKey[i].right - S(114),
                                             g_st->rcKey[i].top - S(20), g_st->rcKey[i].right,
                                             g_st->rcKey[i].top - S(2)};
                }
            } else {
                g_st->rcModel[i] = RECT{fx, card.top + S(130), fx + fw, card.top + S(130) + S(34)};
                g_st->rcKey[i] = RECT{};
            }
        }
        if (measDc) {
            SelectObject(measDc, oldF);
            ReleaseDC(g_st->hwnd, measDc);
        }
        g_st->rcAdd = RECT{pad, addY, pad + contentW, addY + S(42)};
    } else if (g_st->tab == TAB_IMGSEARCH) {
        int secY = contentTop + off;
        int engCardY = secY + S(48);
        int engCardH = S(132);
        g_st->rcImgEngineCard = RECT{pad, engCardY, pad + contentW, engCardY + engCardH};

        int fx = pad + S(16);
        int fw = contentW - S(32);
        int gapX = S(10);
        int gapY = S(8);
        int pillW = (fw - gapX * 2) / 3;
        int pillH = S(34);
        int row0Y = engCardY + S(46);
        int row1Y = row0Y + pillH + gapY;
        for (int i = 0; i < IMG_ENGINE_COUNT; ++i) {
            int col = i % 3;
            int row = i / 3;
            int px = fx + col * (pillW + gapX);
            int px2 = (col == 2) ? (fx + fw) : (px + pillW);
            int py = (row == 0) ? row0Y : row1Y;
            g_st->rcImgEngines[i] = RECT{px, py, px2, py + pillH};
        }

        int keyCardY = engCardY + engCardH + S(12);
        int keyCardH = S(116);
        g_st->rcImgbbCard = RECT{pad, keyCardY, pad + contentW, keyCardY + keyCardH};
        g_st->rcImgbbKey = RECT{fx, keyCardY + S(66), fx + fw, keyCardY + S(66) + S(34)};
        g_st->rcImgbbGetKey = RECT{g_st->rcImgbbKey.right - S(114), g_st->rcImgbbKey.top - S(20),
                                   g_st->rcImgbbKey.right, g_st->rcImgbbKey.top - S(2)};

        int hintY = keyCardY + keyCardH + S(12);
        int hintH = S(64);
        g_st->rcImgHint = RECT{pad, hintY, pad + contentW, hintY + hintH};
    } else {
        int secY = contentTop + off;
        int dirCardY = secY + S(48);
        int dirCardH = S(104);
        int fx = pad + S(16);
        int fw = contentW - S(32);
        int btnOpenW = S(74);
        int btnPickW = S(84);
        int gap = S(8);
        int rowY = dirCardY + S(50);
        int rowH = S(36);

        g_st->rcOpenDir = RECT{fx + fw - btnOpenW, rowY, fx + fw, rowY + rowH};
        g_st->rcPickDir = RECT{g_st->rcOpenDir.left - gap - btnPickW, rowY,
                               g_st->rcOpenDir.left - gap, rowY + rowH};
        g_st->rcSaveDirBox = RECT{fx, rowY, g_st->rcPickDir.left - gap, rowY + rowH};

        int optCardY = dirCardY + dirCardH + S(12);
        int optCardH = S(124);
        int row1Mid = optCardY + S(31);
        int row2Mid = optCardY + S(62) + S(31);
        int tx = pad + contentW - S(18) - S(36);
        g_st->rcToggleAutoSave = RECT{tx, row1Mid - S(10), tx + S(36), row1Mid + S(10)};
        g_st->rcToggleOpenFolder = RECT{tx, row2Mid - S(10), tx + S(36), row2Mid + S(10)};

        int preY = optCardY + optCardH + S(12);
        g_st->rcSavePreview = RECT{pad, preY, pad + contentW, preY + S(64)};
    }

    int footY = g_st->height - S(kFooterH);
    int bh = S(38);
    int by = footY + (S(kFooterH) - bh) / 2;
    g_st->rcReset = RECT{pad, by, pad + S(112), by + bh};
    g_st->rcSave = RECT{};
    g_st->rcCancel = RECT{};
    g_st->rcClose = RECT{g_st->width - pad - S(28), S(12), g_st->width - pad, S(40)};

    // 保存目录输入框（仅 保存设置 Tab 显示）
    if (g_st->hSaveDir) {
        const RECT& sr = g_st->rcSaveDirBox;
        bool visible = (g_st->tab == TAB_SAVE) && sr.right > sr.left &&
                       sr.bottom + S(2) <= viewBottom && sr.top - S(2) >= viewTop;
        if (visible) {
            int insetY = S(7);
            SetWindowPos(g_st->hSaveDir, HWND_TOP, sr.left + S(10), sr.top + insetY,
                         sr.right - sr.left - S(20), sr.bottom - sr.top - insetY * 2,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(g_st->hSaveDir, SW_HIDE);
        }
    }

    // ImgBB API Key 输入框（仅 以图搜图 Tab 显示）
    if (g_st->hImgbbKey) {
        const RECT& kr = g_st->rcImgbbKey;
        bool visible = (g_st->tab == TAB_IMGSEARCH) && kr.right > kr.left &&
                       kr.bottom + S(2) <= viewBottom && kr.top - S(2) >= viewTop;
        if (visible) {
            int insetY = S(6);
            SetWindowPos(g_st->hImgbbKey, HWND_TOP, kr.left + S(10), kr.top + insetY,
                         kr.right - kr.left - S(20), kr.bottom - kr.top - insetY * 2,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ShowWindow(g_st->hImgbbKey, SW_HIDE);
        }
    }

    // 翻译服务子输入框（仅 翻译服务 Tab 且落在可视区才显示）
    for (size_t i = 0; i < g_cfg.providers.size(); ++i) {
        HWND hs[3] = {i < g_st->hBase.size() ? g_st->hBase[i] : NULL,
                      i < g_st->hModel.size() ? g_st->hModel[i] : NULL,
                      i < g_st->hKey.size() ? g_st->hKey[i] : NULL};
        bool tabOk = (g_st->tab == TAB_TRANSLATE);
        RECT rs[3] = {i < g_st->rcBase.size() ? g_st->rcBase[i] : RECT{},
                      i < g_st->rcModel.size() ? g_st->rcModel[i] : RECT{},
                      i < g_st->rcKey.size() ? g_st->rcKey[i] : RECT{}};
        for (int k = 0; k < 3; ++k) {
            if (!hs[k]) continue;
            bool visible = tabOk && rs[k].bottom + S(2) <= viewBottom && rs[k].top - S(2) >= viewTop;
            if (visible) {
                int insetY = S(6);
                SetWindowPos(hs[k], HWND_TOP, rs[k].left + S(10), rs[k].top + insetY,
                             rs[k].right - rs[k].left - S(20), rs[k].bottom - rs[k].top - insetY * 2,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
            } else {
                ShowWindow(hs[k], SW_HIDE);
            }
        }
    }
}

int HitTest(POINT p) {
    if (!g_st) return ID_NONE;
    if (PtIn(g_st->rcClose, p)) return ID_CLOSE;
    for (int t = 0; t < kTabCount; ++t)
        if (PtIn(g_st->rcTab[t], p)) return ID_TAB0 + t;

    int footY = g_st->height - S(kFooterH);
    if (PtIn(g_st->rcReset, p)) return ID_RESET;

    // 内容区命中受可视区 [contentTop, footY) 约束
    if (p.y < g_st->contentTop || p.y >= footY) return ID_NONE;
    if (g_st->scrollMax > 0 && PtIn(g_st->rcScrollTrack, p)) return ID_SCROLLBAR;

    if (g_st->tab == TAB_HOTKEY) {
        for (int i = 0; i < kHotCount; ++i)
            if (PtIn(g_st->rcHot[i], p)) return ID_HOTKEY0 + i;
    } else if (g_st->tab == TAB_TRANSLATE) {
        if (PtIn(g_st->rcSelectMaster, p)) return ID_TOGGLE_SELECT_MASTER;
        if (PtIn(g_st->rcLangForeign, p)) return ID_LANG_FOREIGN;
        if (PtIn(g_st->rcLangChinese, p)) return ID_LANG_CHINESE;
        for (size_t i = 0; i < g_cfg.providers.size(); ++i) {
            if (i < g_st->rcToggle.size() && PtIn(g_st->rcToggle[i], p)) return ID_TOGGLE0 + (int)i;
            if (i < g_st->rcSelect.size() && PtIn(g_st->rcSelect[i], p)) return ID_SELECT0 + (int)i;
            if (i < g_st->rcGetKey.size() && PtIn(g_st->rcGetKey[i], p)) return ID_GETKEY0 + (int)i;
            if (i < g_st->rcDel.size() && PtIn(g_st->rcDel[i], p)) return ID_DEL0 + (int)i;
        }
        if (PtIn(g_st->rcAdd, p)) return ID_ADD;
    } else if (g_st->tab == TAB_IMGSEARCH) {
        for (int i = 0; i < IMG_ENGINE_COUNT; ++i)
            if (PtIn(g_st->rcImgEngines[i], p)) return ID_IMGENGINE0 + i;
        if (PtIn(g_st->rcImgbbGetKey, p)) return ID_GETKEY_IMGBB;
    } else {
        if (PtIn(g_st->rcPickDir, p)) return ID_PICK_DIR;
        if (PtIn(g_st->rcOpenDir, p)) return ID_OPEN_DIR;
        if (PtIn(g_st->rcToggleAutoSave, p)) return ID_TOGGLE_AUTOSAVE;
        if (PtIn(g_st->rcToggleOpenFolder, p)) return ID_TOGGLE_OPENFOLDER;
    }
    return ID_NONE;
}

// ---------------------------------------------------------------- 绘制

void PaintButton(HDC dc, const RECT& rc, const std::wstring& text, HFONT font, float hover,
                 bool primary, bool pressed) {
    RECT r = rc;
    if (pressed) OffsetRect(&r, 0, 1);
    if (primary) {
        COLORREF bg = MixColor(ui::Accent(), ui::AccentHover(), hover);
        if (pressed) bg = RGB(214, 126, 12);
        ui::RoundRect(dc, r, S(7), bg, CLR_INVALID);
        ui::Text(dc, text, r, RGB(255, 255, 255), font,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        COLORREF bg = MixColor(ui::Bg(), ui::Panel(), hover);
        ui::RoundRect(dc, r, S(7), bg, ui::Border());
        ui::Text(dc, text, r, RGB(74, 74, 74), font,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void PaintClose(HDC dc, const RECT& rc, float hover) {
    if (hover > 0.01f)
        ui::RoundRect(dc, rc, S(6), MixColor(ui::Bg(), RGB(240, 241, 244), hover), CLR_INVALID);
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    int arm = S(5);
    HPEN pen = CreatePen(PS_SOLID, 1, MixColor(ui::SubText(), ui::Text(), hover));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, cx - arm, cy - arm, NULL);
    LineTo(dc, cx + arm + 1, cy + arm + 1);
    MoveToEx(dc, cx + arm, cy - arm, NULL);
    LineTo(dc, cx - arm - 1, cy + arm + 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void PaintTabs(HDC dc) {
    if (!g_st) return;
    ui::RoundRect(dc, g_st->rcTabBar, S(10), ui::Panel(), ui::Hairline());
    for (int t = 0; t < kTabCount; ++t) {
        const RECT& r = g_st->rcTab[t];
        bool active = (g_st->tab == t);
        bool hovered = (g_st->hover == ID_TAB0 + t);
        if (active) {
            ui::RoundRect(dc, r, S(7), RGB(255, 255, 255), RGB(231, 233, 238));
        } else if (hovered || g_st->aTabHover[t] > 0.01f) {
            ui::RoundRect(dc, r, S(7), MixColor(ui::Panel(), RGB(255, 255, 255), g_st->aTabHover[t]),
                          CLR_INVALID);
        }
        ui::Text(dc, kTabLabels[t], r, active ? ui::Text() : ui::SubText(),
                 active ? g_st->fTab : g_st->fSmall,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void PaintImgSearchTab(HDC dc, int contentTop) {
    const int pad = S(kPad);
    const int W = g_st->width;
    const int contentW = W - pad * 2;
    int secY = contentTop - g_st->scroll;

    ui::TextLine(dc, L"以图搜图", pad, secY, S(240), S(22), ui::Text(), g_st->fSection);
    ui::TextLine(dc, L"将截图上传为临时直链并调用搜索引擎找高清原图、相似图或番剧出处。", pad,
                 secY + S(24), contentW, S(18), ui::SubText(), g_st->fSmall);

    // 卡片 1：默认识图厂家
    const RECT& engCard = g_st->rcImgEngineCard;
    ui::RoundRect(dc, engCard, S(12), ui::Panel(), ui::Hairline());
    ui::TextLine(dc, L"默认识图厂家", engCard.left + S(16), engCard.top + S(14), S(160), S(22),
                 ui::Text(), g_st->fLabel);

    int descIdx = g_cfg.imageSearchEngine;
    if (g_st->hover >= ID_IMGENGINE0 && g_st->hover < ID_IMGENGINE0 + IMG_ENGINE_COUNT) {
        descIdx = g_st->hover - ID_IMGENGINE0;
    }
    ui::TextLine(dc, img_search::EngineDesc(descIdx), engCard.left + S(160), engCard.top + S(16),
                 (engCard.right - S(16)) - (engCard.left + S(160)), S(18), ui::SubText(),
                 g_st->fSmall, DT_RIGHT);

    for (int i = 0; i < IMG_ENGINE_COUNT; ++i) {
        RECT r = g_st->rcImgEngines[i];
        bool sel = (g_cfg.imageSearchEngine == i);
        bool hov = (g_st->hover == ID_IMGENGINE0 + i);
        if (g_st->pressed == ID_IMGENGINE0 + i) OffsetRect(&r, 0, 1);

        COLORREF fill = sel ? ui::AccentSoft() : (hov ? RGB(255, 255, 255) : ui::Field());
        COLORREF border = sel ? ui::Accent() : (hov ? RGB(200, 205, 214) : ui::Border());
        ui::RoundRect(dc, r, S(8), fill, border);

        int dotD = S(8);
        int dotX = r.left + S(12);
        int dotY = (r.top + r.bottom - dotD) / 2;
        if (sel) {
            HBRUSH db = CreateSolidBrush(ui::Accent());
            HPEN dp = CreatePen(PS_SOLID, 1, ui::Accent());
            HGDIOBJ ob = SelectObject(dc, db);
            HGDIOBJ op = SelectObject(dc, dp);
            Ellipse(dc, dotX, dotY, dotX + dotD, dotY + dotD);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(db);
            DeleteObject(dp);
        } else {
            HPEN dp = CreatePen(PS_SOLID, 1, hov ? RGB(170, 176, 186) : ui::Border());
            HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            HGDIOBJ op = SelectObject(dc, dp);
            Ellipse(dc, dotX, dotY, dotX + dotD, dotY + dotD);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(dp);
        }

        RECT tr{dotX + dotD + S(8), r.top, r.right - S(10), r.bottom};
        ui::Text(dc, img_search::EngineLabel(i), tr, sel ? RGB(176, 92, 6) : ui::Text(),
                 sel ? g_st->fBtn : g_st->fField,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (i == IMG_ENGINE_YANDEX) {
            RECT tagRc{r.right - S(46), r.top, r.right - S(10), r.bottom};
            ui::Text(dc, L"推荐", tagRc, sel ? ui::Accent() : RGB(156, 162, 172), g_st->fSmall,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    // 卡片 2：ImgBB 直链图床配置
    const RECT& keyCard = g_st->rcImgbbCard;
    ui::RoundRect(dc, keyCard, S(12), ui::Panel(), ui::Hairline());
    ui::TextLine(dc, L"ImgBB 直链图床", keyCard.left + S(16), keyCard.top + S(14), S(130), S(22),
                 ui::Text(), g_st->fLabel);

    bool missingImgbb = g_cfg.imgbbKey.empty();
    if (missingImgbb) {
        RECT badge{keyCard.left + S(136), keyCard.top + S(15), keyCard.left + S(360),
                   keyCard.top + S(35)};
        ui::RoundRect(dc, badge, S(5), RGB(255, 246, 232), RGB(250, 204, 145));
        ui::Text(dc, L"当前未配置 Key · 请点击右下免费获取创建", badge, RGB(194, 98, 8),
                 g_st->fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        RECT badge{keyCard.left + S(136), keyCard.top + S(15), keyCard.left + S(312),
                   keyCard.top + S(35)};
        ui::RoundRect(dc, badge, S(5), RGB(232, 247, 237), RGB(196, 233, 209));
        ui::Text(dc, L"海外高速直链 · 10分钟自动销毁", badge, RGB(34, 138, 72), g_st->fSmall,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    ui::TextLine(dc, L"API Key", g_st->rcImgbbKey.left, g_st->rcImgbbKey.top - S(18), S(90), S(16),
                 ui::SubText(), g_st->fSmall);
    bool hovLink = (g_st->hover == ID_GETKEY_IMGBB);
    ui::Text(dc, L"免费获取API Key", g_st->rcImgbbGetKey,
             hovLink ? ui::AccentHover() : ui::Accent(), g_st->fSmall,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    ui::RoundRect(dc, g_st->rcImgbbKey, S(6), ui::Field(),
                  missingImgbb ? RGB(242, 168, 72) : ui::Border());
    if (!g_st->hImgbbKey || !IsWindowVisible(g_st->hImgbbKey)) {
        bool hasAny = !g_cfg.imgbbKey.empty();
        if (g_st->hImgbbKey) {
            wchar_t kb[64] = {};
            GetWindowTextW(g_st->hImgbbKey, kb, 64);
            hasAny = (kb[0] != L'\0');
        }
        if (hasAny) {
            RECT kr{g_st->rcImgbbKey.left + S(10), g_st->rcImgbbKey.top,
                    g_st->rcImgbbKey.right - S(10), g_st->rcImgbbKey.bottom};
            ui::Text(dc, kMaskedKey, kr, ui::SubText(), g_st->fField,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    // 卡片 3：操作提示
    ui::RoundRect(dc, g_st->rcImgHint, S(10), ui::Panel(), CLR_INVALID);
    int ix = g_st->rcImgHint.left + S(16);
    int iy = g_st->rcImgHint.top + S(14);
    int d = S(18);
    HPEN pen = CreatePen(PS_SOLID, S(2), ui::Accent());
    HGDIOBJ old = SelectObject(dc, pen);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, ix, iy, ix + d, iy + d);
    SelectObject(dc, oldB);
    SelectObject(dc, old);
    DeleteObject(pen);
    ui::TextLine(dc, L"i", ix, iy - S(1), d, d, ui::Accent(), g_st->fSmall, DT_CENTER);

    int tx = ix + d + S(12);
    int tw = g_st->rcImgHint.right - tx - S(16);
    ui::TextLine(dc, L"单击工具栏「识图」图标或按 S 直接用默认厂家搜图并关闭截图窗", tx,
                 g_st->rcImgHint.top + S(12), tw, S(20), RGB(96, 96, 102), g_st->fSmall);
    ui::TextLine(dc, L"鼠标悬停在工具栏「识图」图标上，可在下方弹出栏一键切换其他厂家搜图", tx,
                 g_st->rcImgHint.top + S(34), tw, S(20), ui::SubText(), g_st->fSmall);
}

void PaintSaveTab(HDC dc, int contentTop) {
    const int pad = S(kPad);
    const int W = g_st->width;
    const int contentW = W - pad * 2;
    int secY = contentTop - g_st->scroll;

    ui::TextLine(dc, L"截图保存与留存", pad, secY, S(300), S(22), ui::Text(), g_st->fSection);
    ui::TextLine(dc, L"每次截图按当天日期分文件夹留存（前缀 Zpix），下载后可自动定位目录。",
                 pad, secY + S(24), contentW, S(18), ui::SubText(), g_st->fSmall);

    // 卡片 1：保存主目录
    int dirCardY = secY + S(48);
    RECT dirCard{pad, dirCardY, pad + contentW, dirCardY + S(104)};
    ui::RoundRect(dc, dirCard, S(12), ui::Panel(), ui::Hairline());

    ui::TextLine(dc, L"保存主目录", dirCard.left + S(16), dirCardY + S(16), S(160), S(22),
                 ui::Text(), g_st->fLabel);
    ui::TextLine(dc, L"留空则使用默认“图片\\Zpix”", dirCard.right - S(236), dirCardY + S(18),
                 S(220), S(18), ui::SubText(), g_st->fSmall, DT_RIGHT);

    ui::RoundRect(dc, g_st->rcSaveDirBox, S(7), ui::Field(), ui::Border());
    PaintButton(dc, g_st->rcPickDir, L"选择目录", g_st->fSmall, g_st->aPickDir, false,
                g_st->pressed == ID_PICK_DIR);
    PaintButton(dc, g_st->rcOpenDir, L"打开", g_st->fSmall, g_st->aOpenDir, false,
                g_st->pressed == ID_OPEN_DIR);

    // 卡片 2：自动留存与下载后打开目录
    int optCardY = dirCard.bottom + S(12);
    RECT optCard{pad, optCardY, pad + contentW, optCardY + S(124)};
    ui::RoundRect(dc, optCard, S(12), ui::Panel(), ui::Hairline());

    // 行 1：每次截图自动留存
    ui::TextLine(dc, L"每次截图自动留存到本地目录", optCard.left + S(18), optCardY + S(12),
                 contentW - S(90), S(20), ui::Text(), g_st->fLabel);
    ui::TextLine(dc, L"复制、贴图、文字识别、翻译或下载时自动保存，每天按日期归档",
                 optCard.left + S(18), optCardY + S(34), contentW - S(90), S(18), ui::SubText(),
                 g_st->fSmall);
    ui::Toggle(dc, g_st->rcToggleAutoSave, g_st->aAutoSave, g_st->hover == ID_TOGGLE_AUTOSAVE);

    // 分割线
    {
        RECT hair{optCard.left + S(18), optCardY + S(62), optCard.right - S(18),
                  optCardY + S(62) + 1};
        HBRUSH hb = CreateSolidBrush(ui::Hairline());
        FillRect(dc, &hair, hb);
        DeleteObject(hb);
    }

    // 行 2：下载后自动打开对应文件夹
    ui::TextLine(dc, L"点击下载完成后自动打开对应文件夹", optCard.left + S(18),
                 optCardY + S(62) + S(12), contentW - S(90), S(20), ui::Text(), g_st->fLabel);
    ui::TextLine(dc, L"点击工具栏“下载”或按 Ctrl+S 保存后，自动打开当天目录并选中该图片",
                 optCard.left + S(18), optCardY + S(62) + S(34), contentW - S(90), S(18),
                 ui::SubText(), g_st->fSmall);
    ui::Toggle(dc, g_st->rcToggleOpenFolder, g_st->aOpenFolder,
               g_st->hover == ID_TOGGLE_OPENFOLDER);

    // 卡片 3：实时路径示例
    ui::RoundRect(dc, g_st->rcSavePreview, S(10), ui::Panel(), CLR_INVALID);
    int ix = g_st->rcSavePreview.left + S(16);
    int iy = g_st->rcSavePreview.top + S(14);
    int d = S(18);
    HPEN pen = CreatePen(PS_SOLID, S(2), ui::Accent());
    HGDIOBJ old = SelectObject(dc, pen);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, ix, iy, ix + d, iy + d);
    SelectObject(dc, oldB);
    SelectObject(dc, old);
    DeleteObject(pen);
    ui::TextLine(dc, L"i", ix, iy - S(1), d, d, ui::Accent(), g_st->fSmall, DT_CENTER);

    int tx = ix + d + S(12);
    int tw = g_st->rcSavePreview.right - tx - S(16);
    ui::TextLine(dc, L"归档结构：主目录 \\ YYYY-MM-DD \\ Zpix_YYYYMMDD_HHMMSS.png", tx,
                 g_st->rcSavePreview.top + S(12), tw, S(20), RGB(96, 96, 102), g_st->fSmall);
    std::wstring preview = L"当前示例：" + PreviewDailyShotPath(g_cfg.saveDir);
    RECT pr{tx, g_st->rcSavePreview.top + S(34), tx + tw, g_st->rcSavePreview.top + S(54)};
    ui::Text(dc, preview, pr, ui::SubText(), g_st->fSmall,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
}

void PaintHotkeyTab(HDC dc, int contentTop) {
    const int pad = S(kPad);
    const int W = g_st->width;
    int secY = contentTop - g_st->scroll;
    ui::TextLine(dc, L"快捷键设置", pad, secY, S(300), S(22), ui::Text(), g_st->fSection);
    ui::TextLine(dc, L"自定义常用操作的键盘快捷键，让操作更加高效。", pad, secY + S(24), W - pad * 2,
                 S(18), ui::SubText(), g_st->fSmall);

    int cardTop = secY + S(48);
    RECT card{pad, cardTop, W - pad, cardTop + S(kRowH) * kHotCount + S(12)};
    ui::RoundRect(dc, card, S(12), ui::Panel(), ui::Hairline());

    for (int i = 0; i < kHotCount; ++i) {
        int ry = cardTop + S(6) + i * S(kRowH);
        if (i > 0) {
            RECT hair{card.left + S(18), ry, card.right - S(18), ry + 1};
            HBRUSH hb = CreateSolidBrush(ui::Hairline());
            FillRect(dc, &hair, hb);
            DeleteObject(hb);
        }
        ui::TextLine(dc, kHotLabels[i], card.left + S(18), ry, card.right - card.left - S(200),
                     S(kRowH), ui::Text(), g_st->fLabel);

        const RECT& f = g_st->rcHot[i];
        bool capturing = g_st->capture == i;
        bool hovering = g_st->hover == ID_HOTKEY0 + i;
        COLORREF fill = capturing ? ui::AccentSoft() : ui::Field();
        COLORREF border = (capturing || hovering) ? ui::Accent() : ui::Border();
        ui::RoundRect(dc, f, S(6), fill, border);

        std::wstring txt;
        COLORREF txtColor = ui::Text();
        if (capturing) {
            txt = L"请按下快捷键";
            txtColor = ui::Accent();
        } else {
            std::wstring* v = HotPtr(i);
            if (v) txt = *v;
        }
        ui::TextLine(dc, txt, f.left, f.top, f.right - f.left, f.bottom - f.top, txtColor,
                     capturing ? g_st->fSmall : g_st->fField, DT_CENTER);
    }

    ui::RoundRect(dc, g_st->rcHint, S(10), ui::Panel(), CLR_INVALID);
    int ix = g_st->rcHint.left + S(16);
    int iy = g_st->rcHint.top + S(14);
    int d = S(18);
    HPEN pen = CreatePen(PS_SOLID, S(2), ui::Accent());
    HGDIOBJ old = SelectObject(dc, pen);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, ix, iy, ix + d, iy + d);
    SelectObject(dc, oldB);
    SelectObject(dc, old);
    DeleteObject(pen);
    ui::TextLine(dc, L"i", ix, iy - S(1), d, d, ui::Accent(), g_st->fSmall, DT_CENTER);

    int tx = ix + d + S(12);
    ui::TextLine(dc, L"动作快捷键仅在截图选区出现后生效；截图热键为全局热键。", tx,
                 g_st->rcHint.top + S(12), g_st->rcHint.right - tx - S(16), S(20), RGB(96, 96, 102),
                 g_st->fSmall);
    ui::TextLine(dc, L"点击右侧按键位即可重新录制，Esc 取消、Tab 切换到下一行。", tx,
                 g_st->rcHint.top + S(34), g_st->rcHint.right - tx - S(16), S(20), ui::SubText(),
                 g_st->fSmall);
}

void PaintTranslateTab(HDC dc, int contentTop) {
    const int pad = S(kPad);
    const int W = g_st->width;
    int secY = contentTop - g_st->scroll;
    ui::TextLine(dc, L"翻译服务", pad, secY, S(240), S(22), ui::Text(), g_st->fSection);

    // 右上角：全局划词翻译总开关
    ui::TextLine(dc, L"全局划词翻译", g_st->rcSelectMaster.left - S(110), secY + S(2), S(102),
                 S(20), ui::Text(), g_st->fSmall, DT_RIGHT);
    ui::Toggle(dc, g_st->rcSelectMaster, g_st->aSelectMaster,
               g_st->hover == ID_TOGGLE_SELECT_MASTER);

    ui::TextLine(dc, L"源语言自动识别；外语与中文自动双向互译，点击名称旁「划词翻译」可多选。",
                 pad, secY + S(24), W - pad * 2, S(18), ui::SubText(), g_st->fSmall);

    // 默认目标语言双向互译卡片
    {
        const RECT& lc = g_st->rcLangCard;
        ui::RoundRect(dc, lc, S(12), ui::Panel(), ui::Hairline());
        int midX = (lc.left + lc.right) / 2;

        ui::TextLine(dc, L"外语默认译为", lc.left + S(16), lc.top,
                     g_st->rcLangForeign.left - lc.left - S(22), lc.bottom - lc.top, ui::Text(),
                     g_st->fLabel);
        bool hovF = (g_st->hover == ID_LANG_FOREIGN);
        ui::RoundRect(dc, g_st->rcLangForeign, S(6), ui::Field(),
                      hovF ? ui::Accent() : ui::Border());
        ui::Text(dc, LangDisplayName(g_cfg.targetLang) + L"  ▾", g_st->rcLangForeign,
                 hovF ? ui::Accent() : ui::Text(), g_st->fSmall,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT sep{midX, lc.top + S(12), midX + 1, lc.bottom - S(12)};
        HBRUSH hb = CreateSolidBrush(ui::Hairline());
        FillRect(dc, &sep, hb);
        DeleteObject(hb);

        ui::TextLine(dc, L"中文默认译为", midX + S(16), lc.top,
                     g_st->rcLangChinese.left - midX - S(22), lc.bottom - lc.top, ui::Text(),
                     g_st->fLabel);
        bool hovC = (g_st->hover == ID_LANG_CHINESE);
        ui::RoundRect(dc, g_st->rcLangChinese, S(6), ui::Field(),
                      hovC ? ui::Accent() : ui::Border());
        ui::Text(dc, LangDisplayName(g_cfg.cnTargetLang) + L"  ▾", g_st->rcLangChinese,
                 hovC ? ui::Accent() : ui::Text(), g_st->fSmall,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    for (size_t i = 0; i < g_cfg.providers.size() && i < g_st->rcCard.size(); ++i) {
        const Provider& p = g_cfg.providers[i];
        const RECT& c = g_st->rcCard[i];
        ui::RoundRect(dc, c, S(12), ui::Panel(), ui::Hairline());

        ui::Toggle(dc, g_st->rcToggle[i], i < g_st->aToggle.size() ? g_st->aToggle[i] : 0.0f,
                   g_st->hover == ID_TOGGLE0 + (int)i);

        int nx = g_st->rcToggle[i].right + S(12);
        int nameMaxW = (i < g_st->rcSelect.size() ? g_st->rcSelect[i].left - nx - S(6) : S(300));
        if (nameMaxW < S(80)) nameMaxW = S(80);
        ui::TextLine(dc, p.name.empty() ? ProviderKindLabel(p.kind) : p.name, nx, c.top + S(8),
                     nameMaxW, S(24), ui::Text(), g_st->fLabel);

        // 厂家名称旁：「划词翻译」启用状态按钮
        if (i < g_st->rcSelect.size()) {
            const RECT& sr = g_st->rcSelect[i];
            bool on = p.selectEnabled;
            bool hov = (g_st->hover == ID_SELECT0 + (int)i);
            COLORREF fill = on ? ui::AccentSoft() : (hov ? RGB(241, 243, 246) : RGB(255, 255, 255));
            COLORREF bd = on ? ui::Accent() : (hov ? RGB(198, 204, 214) : ui::Border());
            ui::RoundRect(dc, sr, S(11), fill, bd);

            int dotD = S(6);
            int dotX = sr.left + S(9);
            int dotY = (sr.top + sr.bottom - dotD) / 2;
            RECT dotRc{dotX, dotY, dotX + dotD, dotY + dotD};
            ui::RoundRect(dc, dotRc, dotD / 2, on ? ui::Accent() : RGB(190, 196, 206), CLR_INVALID);

            RECT lblRc{dotRc.right + S(5), sr.top, sr.right - S(6), sr.bottom};
            ui::Text(dc, L"划词翻译", lblRc, on ? ui::Accent() : ui::SubText(), g_st->fSmall,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        std::wstring tag = (p.kind == TrKind::Zhipu)
                               ? L"当前模型为免费模型"
                               : ((p.kind == TrKind::Index) ? L"免费公网模型" : ProviderKindLabel(p.kind));
        SIZE tagSz{};
        HGDIOBJ oldTf = SelectObject(dc, g_st->fSmall);
        GetTextExtentPoint32W(dc, tag.c_str(), (int)tag.size(), &tagSz);
        SelectObject(dc, oldTf);
        int tagW = tagSz.cx + S(16);
        RECT tagRc{nx, c.top + S(33), nx + tagW, c.top + S(33) + S(19)};
        bool isZhipuFree = (p.kind == TrKind::Zhipu);
        ui::RoundRect(dc, tagRc, S(4),
                      isZhipuFree ? RGB(236, 253, 243) : RGB(255, 255, 255),
                      isZhipuFree ? RGB(167, 243, 208) : ui::Border());
        ui::Text(dc, tag, tagRc, isZhipuFree ? RGB(22, 148, 70) : ui::SubText(), g_st->fSmall,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (p.kind == TrKind::Zhipu && p.key.empty()) {
            const wchar_t* warnTxt = L"未配置 Key · 默认已关闭（填入后自动启用）";
            SIZE wsz{};
            HGDIOBJ wf = SelectObject(dc, g_st->fSmall);
            GetTextExtentPoint32W(dc, warnTxt, (int)wcslen(warnTxt), &wsz);
            SelectObject(dc, wf);
            int wx = tagRc.right + S(8);
            RECT wrc{wx, c.top + S(33), wx + wsz.cx + S(16), c.top + S(33) + S(19)};
            ui::RoundRect(dc, wrc, S(4), RGB(255, 246, 232), RGB(250, 204, 145));
            ui::Text(dc, warnTxt, wrc, RGB(194, 98, 8), g_st->fSmall,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        if (!IsBuiltInProvider(p.kind)) {
            bool delHover = g_st->hover == ID_DEL0 + (int)i;
            ui::Text(dc, L"删除", g_st->rcDel[i], delHover ? RGB(208, 74, 74) : ui::SubText(),
                     g_st->fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        auto drawFallbackText = [&](const RECT& r, const std::wstring& txt, HWND hEdit) {
            if (hEdit && IsWindowVisible(hEdit)) return;
            std::wstring show = txt;
            if (hEdit) {
                wchar_t buf[2048] = {};
                GetWindowTextW(hEdit, buf, 2048);
                show = buf;
            }
            if (show.empty()) return;
            RECT tr{r.left + S(10), r.top, r.right - S(10), r.bottom};
            ui::Text(dc, show, tr, ui::Text(), g_st->fField,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        };

        ui::TextLine(dc, L"接口地址", g_st->rcBase[i].left, g_st->rcBase[i].top - S(18), S(100), S(16),
                     ui::SubText(), g_st->fSmall);
        ui::RoundRect(dc, g_st->rcBase[i], S(6), ui::Field(), ui::Border());
        drawFallbackText(g_st->rcBase[i], p.base, i < g_st->hBase.size() ? g_st->hBase[i] : NULL);

        ui::TextLine(dc, L"模型", g_st->rcModel[i].left, g_st->rcModel[i].top - S(18), S(100), S(16),
                     ui::SubText(), g_st->fSmall);
        if (IsModelLockedProvider(p.kind)) {
            ui::RoundRect(dc, g_st->rcModel[i], S(6), RGB(242, 244, 247), ui::Hairline());
            RECT valRc{g_st->rcModel[i].left + S(10), g_st->rcModel[i].top,
                       g_st->rcModel[i].right - S(42), g_st->rcModel[i].bottom};
            ui::Text(dc, p.model, valRc, RGB(102, 106, 115), g_st->fField,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            RECT tagFix{g_st->rcModel[i].right - S(40), g_st->rcModel[i].top,
                        g_st->rcModel[i].right - S(10), g_st->rcModel[i].bottom};
            ui::Text(dc, L"固定", tagFix, RGB(164, 168, 178), g_st->fSmall,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            ui::RoundRect(dc, g_st->rcModel[i], S(6), ui::Field(), ui::Border());
            drawFallbackText(g_st->rcModel[i], p.model,
                             i < g_st->hModel.size() ? g_st->hModel[i] : NULL);
        }

        if (HasApiKeyField(p.kind)) {
            ui::TextLine(dc, L"API Key", g_st->rcKey[i].left, g_st->rcKey[i].top - S(18), S(80),
                         S(16), ui::SubText(), g_st->fSmall);
            if (p.kind == TrKind::Zhipu && i < g_st->rcGetKey.size()) {
                bool hovLink = (g_st->hover == ID_GETKEY0 + (int)i);
                ui::Text(dc, L"免费获取API Key", g_st->rcGetKey[i],
                         hovLink ? ui::AccentHover() : ui::Accent(), g_st->fSmall,
                         DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            ui::RoundRect(dc, g_st->rcKey[i], S(6), ui::Field(), ui::Border());
            HWND hk = i < g_st->hKey.size() ? g_st->hKey[i] : NULL;
            if (!hk || !IsWindowVisible(hk)) {
                bool hasAny = !p.key.empty();
                if (hk) {
                    wchar_t kb[64] = {};
                    GetWindowTextW(hk, kb, 64);
                    hasAny = (kb[0] != L'\0');
                }
                if (hasAny) {
                    RECT kr{g_st->rcKey[i].left + S(10), g_st->rcKey[i].top,
                            g_st->rcKey[i].right - S(10), g_st->rcKey[i].bottom};
                    ui::Text(dc, kMaskedKey, kr, ui::SubText(), g_st->fField,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
        }
    }

    float a = g_st->aAdd;
    COLORREF border = MixColor(ui::Border(), ui::Accent(), a);
    COLORREF text = MixColor(RGB(107, 107, 115), ui::Accent(), a);
    RECT r = g_st->rcAdd;
    if (g_st->pressed == ID_ADD) OffsetRect(&r, 0, 1);
    ui::RoundRect(dc, r, S(10), MixColor(ui::Bg(), ui::AccentSoft(), a * 0.55f), border);
    ui::Text(dc, L"+    添加翻译厂家", r, text, g_st->fBtn,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void Paint(HDC dc) {
    if (!g_st) return;
    const int W = g_st->width;
    const int H = g_st->height;
    const int pad = S(kPad);
    const int footY = H - S(kFooterH);

    RECT all{0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(ui::Bg());
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    // 标题栏
    if (g_st->icon) ui::DrawIcon22(dc, g_st->icon, pad, S(16), S(20));
    ui::TextLine(dc, L"设置", pad + S(28), S(12), S(200), S(28), ui::Text(), g_st->fTitle);
    PaintClose(dc, g_st->rcClose, g_st->aClose);

    ui::TextLine(dc, L"自定义你的截图保存、快捷键、翻译与识图体验", pad, S(kTitleH) + S(2), W - pad * 2,
                 S(20), ui::SubText(), g_st->fSub);

    PaintTabs(dc);

    // 内容区裁剪（支持长列表滚轮滚动，不遮挡标题栏与底部栏）
    int contentTop = g_st->rcTabBar.bottom + S(20);
    int saved = SaveDC(dc);
    IntersectClipRect(dc, 0, contentTop, W, footY);
    if (g_st->tab == TAB_HOTKEY)
        PaintHotkeyTab(dc, contentTop);
    else if (g_st->tab == TAB_TRANSLATE)
        PaintTranslateTab(dc, contentTop);
    else if (g_st->tab == TAB_IMGSEARCH)
        PaintImgSearchTab(dc, contentTop);
    else
        PaintSaveTab(dc, contentTop);
    RestoreDC(dc, saved);

    // 滚动条指示（仅当内容超出可视高度时显示，支持滚轮与拖拽）
    if (g_st->scrollMax > 0 && g_st->rcScrollThumb.bottom > g_st->rcScrollThumb.top) {
        int bw = g_st->rcScrollThumb.right - g_st->rcScrollThumb.left;
        if (g_st->hover == ID_SCROLLBAR || g_st->scrollDrag) {
            RECT trk{g_st->rcScrollThumb.left, g_st->rcScrollTrack.top,
                     g_st->rcScrollThumb.right, g_st->rcScrollTrack.bottom};
            ui::RoundRect(dc, trk, bw / 2, RGB(240, 242, 246), CLR_INVALID);
        }
        COLORREF thumbCol = g_st->scrollDrag
                                ? RGB(154, 160, 172)
                                : (g_st->hover == ID_SCROLLBAR ? RGB(180, 186, 196)
                                                               : RGB(206, 211, 220));
        ui::RoundRect(dc, g_st->rcScrollThumb, bw / 2, thumbCol, CLR_INVALID);
    }

    // 底部
    {
        RECT hair{0, footY, W, footY + 1};
        HBRUSH hb = CreateSolidBrush(ui::Hairline());
        FillRect(dc, &hair, hb);
        DeleteObject(hb);
    }
    PaintButton(dc, g_st->rcReset, L"恢复默认", g_st->fBtn, g_st->aReset, false,
                g_st->pressed == ID_RESET);
    {
        const wchar_t* autoTip = L"修改实时自动生效";
        SIZE tsz{};
        HGDIOBJ of = SelectObject(dc, g_st->fSmall);
        GetTextExtentPoint32W(dc, autoTip, (int)wcslen(autoTip), &tsz);
        SelectObject(dc, of);
        int dotD = S(7);
        int tipRight = W - pad;
        int txtX = tipRight - tsz.cx;
        int dotX = txtX - S(7) - dotD;
        int midY = footY + S(kFooterH) / 2;
        HBRUSH db = CreateSolidBrush(RGB(42, 168, 88));
        HPEN dp = CreatePen(PS_SOLID, 1, RGB(42, 168, 88));
        HGDIOBJ ob = SelectObject(dc, db);
        HGDIOBJ op = SelectObject(dc, dp);
        Ellipse(dc, dotX, midY - dotD / 2, dotX + dotD, midY - dotD / 2 + dotD);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(db);
        DeleteObject(dp);
        RECT tr{txtX, footY, tipRight, H};
        ui::Text(dc, autoTip, tr, ui::SubText(), g_st->fSmall,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

// ---------------------------------------------------------------- 动画

void UpdateAnim() {
    if (!g_st) return;
    auto step = [](float& v, float target) {
        float d = target - v;
        if (std::fabs(d) < 0.02f) {
            v = target;
            return false;
        }
        v += d * 0.4f;
        return true;
    };
    bool dirty = false;
    dirty |= step(g_st->aSave, g_st->hover == ID_SAVE ? 1.0f : 0.0f);
    dirty |= step(g_st->aCancel, g_st->hover == ID_CANCEL ? 1.0f : 0.0f);
    dirty |= step(g_st->aReset, g_st->hover == ID_RESET ? 1.0f : 0.0f);
    dirty |= step(g_st->aAdd, g_st->hover == ID_ADD ? 1.0f : 0.0f);
    dirty |= step(g_st->aClose, g_st->hover == ID_CLOSE ? 1.0f : 0.0f);
    dirty |= step(g_st->aPickDir, g_st->hover == ID_PICK_DIR ? 1.0f : 0.0f);
    dirty |= step(g_st->aOpenDir, g_st->hover == ID_OPEN_DIR ? 1.0f : 0.0f);
    dirty |= step(g_st->aAutoSave, g_cfg.autoSaveShot ? 1.0f : 0.0f);
    dirty |= step(g_st->aOpenFolder, g_cfg.openFolderAfterSave ? 1.0f : 0.0f);
    dirty |= step(g_st->aSelectMaster, g_cfg.enableSelectTranslate ? 1.0f : 0.0f);
    for (int t = 0; t < kTabCount; ++t)
        if (g_st->tab != t) dirty |= step(g_st->aTabHover[t], g_st->hover == ID_TAB0 + t ? 1.0f : 0.0f);
    for (size_t i = 0; i < g_st->aToggle.size(); ++i) {
        float target = (i < g_cfg.providers.size() && g_cfg.providers[i].enabled) ? 1.0f : 0.0f;
        dirty |= step(g_st->aToggle[i], target);
    }
    if (dirty)
        InvalidateRect(g_st->hwnd, NULL, FALSE);
    else
        KillTimer(g_st->hwnd, kTimerAnim);
}

void KickAnim() {
    if (g_st) SetTimer(g_st->hwnd, kTimerAnim, 15, NULL);
}

void UpdateWindowSize(int mode);

// ---------------------------------------------------------------- 行为

void HideAllEdits() {
    if (!g_st) return;
    if (g_st->hSaveDir) ShowWindow(g_st->hSaveDir, SW_HIDE);
    if (g_st->hImgbbKey) ShowWindow(g_st->hImgbbKey, SW_HIDE);
    for (HWND h : g_st->hBase)
        if (h) ShowWindow(h, SW_HIDE);
    for (HWND h : g_st->hModel)
        if (h) ShowWindow(h, SW_HIDE);
    for (HWND h : g_st->hKey)
        if (h) ShowWindow(h, SW_HIDE);
}

void SwitchTab(int t) {
    if (!g_st || t == g_st->tab) return;
    g_st->tab = t;
    if (g_st->capture >= 0) {
        g_st->capture = -1;
        ApplyHotkeys();
    }
    g_st->scroll = 0;
    HideAllEdits();
    UpdateWindowSize(PlaceKeepTopLeft);
    InvalidateRect(g_st->hwnd, NULL, FALSE);
    UpdateWindow(g_st->hwnd);
    ZLog(L"[set] switch tab=%d", t);
}

void CloseSettingsWindow() {
    if (!g_st) return;
    if (g_st->capture >= 0) {
        g_st->capture = -1;
    }
    PullFieldText(true);
    EnsureProviderDefaults(g_cfg.providers);
    if (!g_cfg.enableSelectTranslate) HideSelectBar();
    if (g_st->hwnd && g_st->savePending) {
        KillTimer(g_st->hwnd, kTimerAutoSave);
    }
    g_st->savePending = false;
    SaveSettings();
    ApplyHotkeys();
    HWND h = g_st->hwnd;
    DestroyWindow(h);
}

void ResetAll() {
    Settings def;
    g_cfg.screenshot = def.screenshot;
    g_cfg.copyImage = def.copyImage;
    g_cfg.pin = def.pin;
    g_cfg.copyText = def.copyText;
    g_cfg.translate = def.translate;
    g_cfg.saveDir = def.saveDir;
    g_cfg.autoSaveShot = def.autoSaveShot;
    g_cfg.openFolderAfterSave = def.openFolderAfterSave;
    g_cfg.enableSelectTranslate = def.enableSelectTranslate;
    g_cfg.imageSearchEngine = def.imageSearchEngine;
    g_cfg.imgbbKey = def.imgbbKey;
    g_cfg.targetLang = def.targetLang;
    g_cfg.cnTargetLang = def.cnTargetLang;
    g_cfg.providers = DefaultProviders();
    if (!g_cfg.enableSelectTranslate) HideSelectBar();
    ApplyHotkeys();
    if (g_st && g_st->hwnd && g_st->savePending) {
        KillTimer(g_st->hwnd, kTimerAutoSave);
    }
    if (g_st) g_st->savePending = false;
    SaveSettings();
    CreateFieldEdits();
    UpdateWindowSize(PlaceKeepTopLeft);
    InvalidateRect(g_st->hwnd, NULL, FALSE);
    ShowToast(L"已恢复默认设置");
    ZLog(L"[cfg] settings reset to defaults");
}

// 判断点 pt 是否已深入其所在显示器内部至少 margin 像素（用于跨屏缝隙防抖，避免在分界线上来回缩放）
bool IsPointInsideMonitorWithMargin(POINT pt, int margin) {
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) return true;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return true;
    return pt.x >= mi.rcMonitor.left + margin && pt.x < mi.rcMonitor.right - margin &&
           pt.y >= mi.rcMonitor.top + margin && pt.y < mi.rcMonitor.bottom - margin;
}

void ComputeDesiredWindowSize(int scale, const RECT& work, int& outW, int& outH) {
    int wantW = MulDiv(kBaseW, scale, 100);
    int maxW = (work.right - work.left) - S(40);
    if (maxW < S(240)) maxW = S(240);
    if (wantW > maxW) wantW = maxW;
    g_st->width = wantW;
    ComputeLayout(false);
    RECT r{0, 0, g_st->width, g_st->height};
    AdjustWindowRectEx(&r, WS_POPUP, FALSE, WS_EX_TOOLWINDOW);
    outW = r.right - r.left;
    outH = r.bottom - r.top;
    int maxH = (work.bottom - work.top) - S(40);
    if (outH > maxH) outH = maxH;
}

void UpdateWindowSize(int mode) {
    if (!g_st) return;

    RECT before{};
    bool had = (mode != PlaceCenterPrimary) && GetWindowRect(g_st->hwnd, &before) &&
               (before.right > before.left);

    POINT probe = had ? POINT{(before.left + before.right) / 2, (before.top + before.bottom) / 2}
                      : POINT{0, 0};
    RECT work = had ? MonitorWorkRect(probe) : PrimaryMonitorWorkRect();
    if (!had) {
        int want = MonitorScaleOf(POINT{work.left + 1, work.top + 1});
        if (want != g_st->scale) {
            g_st->scale = want;
            RebuildFonts();
            ZLog(L"[set] rescale=%d", want);
        }
    }

    int w = 0, h = 0;
    ComputeDesiredWindowSize(g_st->scale, work, w, h);

    int x, y;
    if (mode == PlaceKeepTopLeft && had) {
        x = before.left;
        y = before.top;
        // 切换 Tab / 增删项时保持原位置，仅防止完全跑出虚拟桌面，绝不强拉进单屏导致跨屏跳动
        RECT vs = GetVirtualScreenRect();
        if (x + S(80) > vs.right) x = vs.right - S(80);
        if (x + w - S(80) < vs.left) x = vs.left - w + S(80);
        if (y < vs.top) y = vs.top;
        if (y + S(kTitleH) > vs.bottom) y = vs.bottom - S(kTitleH);
    } else if (mode == PlaceKeepCenter && had) {
        x = (before.left + before.right) / 2 - w / 2;
        y = (before.top + before.bottom) / 2 - h / 2;
    } else {
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
        if (x < work.left) x = work.left;
        if (x + w > work.right) x = work.right - w;
        if (y < work.top) y = work.top;
        if (y + h > work.bottom) y = work.bottom - h;
    }

    g_st->inSizing = true;
    SetWindowPos(g_st->hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    RECT cr{};
    GetClientRect(g_st->hwnd, &cr);
    g_st->height = cr.bottom;
    g_st->width = cr.right;
    ComputeLayout(true);
    g_st->inSizing = false;
    ZLog(L"[set] place at %d,%d %dx%d work=%ld,%ld-%ld,%ld scale=%d mode=%d", x, y, w, h, work.left,
         work.top, work.right, work.bottom, g_st->scale, (int)mode);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_st = new SettingsState();
        g_st->hwnd = hwnd;
        // 缩放比按"窗口最终会落在哪块屏"取，而不是创建瞬间的 DC DPI：
        // 取错会变成 100% 的尺寸配 125% 的字体，右侧内容直接顶出窗口
        RECT work = PrimaryMonitorWorkRect();
        g_st->scale = MonitorScaleOf(POINT{work.left + 1, work.top + 1});
        if (GetDpiForWindow(hwnd)) {
            int s2 = MulDiv((int)GetDpiForWindow(hwnd), 100, 96);
            if (s2 >= 100) g_st->scale = s2;
        }
        RebuildFonts();
        g_st->brushField = CreateSolidBrush(ui::Field());

        g_st->icon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32,
                                       LR_DEFAULTCOLOR);
        g_st->iconOwned = (g_st->icon != nullptr);
        if (!g_st->icon) g_st->icon = LoadIconW(NULL, IDI_APPLICATION);

        typedef HRESULT(WINAPI * DwmSetWindowAttribute_t)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) {
            auto setAttr = (DwmSetWindowAttribute_t)GetProcAddress(dwm, "DwmSetWindowAttribute");
            if (setAttr) {
                int pref = 2;  // DWMWCP_ROUND
                setAttr(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
            }
        }

        g_st->width = MulDiv(kBaseW, g_st->scale, 100);
        g_st->tab = (g_pendingInitialTab >= 0 && g_pendingInitialTab < kTabCount)
                        ? g_pendingInitialTab
                        : 0;
        g_pendingInitialTab = -1;
        CreateFieldEdits();
        UpdateWindowSize(PlaceCenterPrimary);
        // 注意：这里**不**注销全局热键。以前打开设置就 UnregisterHotKey，
        // 结果设置窗口开着的时候怎么按 Ctrl+Alt+A 都截不了图。
        // 只有真正进入"录制新热键"状态时才临时注销（见 WM_LBUTTONDOWN）。
        SetFocus(hwnd);
        return 0;
    }
    case WM_DPICHANGED: {
        // 鼠标自主拖拽期间由 WM_MOUSEMOVE 按光标所在屏平滑处理，忽略半截窗口跨界触发的被动 DPI 消息以防反复横跳
        if (!g_st || g_st->winDragging || g_st->inSizing) return 0;
        UINT dpi = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
        int s2 = dpi ? MulDiv((int)dpi, 100, 96) : g_st->scale;
        if (s2 < 100) s2 = 100;
        const RECT* sug = (const RECT*)lp;
        POINT probe = sug ? POINT{(sug->left + sug->right) / 2, (sug->top + sug->bottom) / 2}
                          : POINT{0, 0};
        RECT work = sug ? MonitorWorkRect(probe) : PrimaryMonitorWorkRect();
        if (s2 != g_st->scale) {
            g_st->scale = s2;
            RebuildFonts();
            ZLog(L"[set] dpi changed scale=%d", s2);
        }
        int w = 0, h = 0;
        ComputeDesiredWindowSize(s2, work, w, h);
        int x = sug ? sug->left : (work.left + (work.right - work.left - w) / 2);
        int y = sug ? sug->top : (work.top + (work.bottom - work.top - h) / 2);
        g_st->inSizing = true;
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        RECT cr{};
        GetClientRect(hwnd, &cr);
        g_st->width = cr.right;
        g_st->height = cr.bottom;
        ComputeLayout(true);
        g_st->inSizing = false;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_SIZE: {
        if (g_st && !g_st->inSizing && wp != SIZE_MINIMIZED) {
            int cw = LOWORD(lp), ch = HIWORD(lp);
            if (cw > 0 && ch > 0 && (cw != g_st->width || ch != g_st->height)) {
                g_st->width = cw;
                g_st->height = ch;
                ComputeLayout(true);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int bw = rc.right > 0 ? rc.right : 1;
        int bh = rc.bottom > 0 ? rc.bottom : 1;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, bw, bh);
        HGDIOBJ old = SelectObject(mem, bmp);
        HBRUSH fullBg = CreateSolidBrush(ui::Bg());
        FillRect(mem, &rc, fullBg);
        DeleteObject(fullBg);
        Paint(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && g_st) InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        HWND ctrl = (HWND)lp;
        COLORREF tc = ui::Text();
        if (g_st) {
            if (g_st->hImgbbKey && ctrl == g_st->hImgbbKey && !g_st->imgbbKeyEdited) {
                tc = ui::SubText();
            }
            for (size_t i = 0; i < g_st->hKey.size(); ++i) {
                if (g_st->hKey[i] && g_st->hKey[i] == ctrl &&
                    i < g_st->keyEdited.size() && !g_st->keyEdited[i]) {
                    tc = ui::SubText();
                    break;
                }
            }
        }
        SetTextColor(dc, tc);
        SetBkColor(dc, ui::Field());
        return (LRESULT)g_st->brushField;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == ID_EDIT_SAVEDIR && g_st && g_st->hSaveDir) {
            if (code == EN_CHANGE) {
                wchar_t buf[2048] = {};
                GetWindowTextW(g_st->hSaveDir, buf, 2048);
                g_cfg.saveDir = NormalizeDirPath(buf);
                InvalidateRect(hwnd, &g_st->rcSavePreview, FALSE);
                ScheduleRealtimeSave();
            } else if (code == EN_KILLFOCUS && g_st->savePending) {
                CommitRealtimeSave(false);
            }
            return 0;
        }
        if (g_st && !g_st->suppressKeyEvent && id == ID_EDIT_IMGBB_KEY && g_st->hImgbbKey) {
            HWND ek = g_st->hImgbbKey;
            if (code == EN_SETFOCUS) {
                if (!g_st->imgbbKeyEdited && !g_cfg.imgbbKey.empty()) {
                    g_st->suppressKeyEvent = true;
                    SetWindowTextW(ek, L"");
                    g_st->suppressKeyEvent = false;
                }
            } else if (code == EN_CHANGE) {
                g_st->imgbbKeyEdited = true;
                InvalidateRect(ek, NULL, FALSE);
                ScheduleRealtimeSave();
            } else if (code == EN_KILLFOCUS) {
                wchar_t buf[2048] = {};
                GetWindowTextW(ek, buf, 2048);
                if (!g_st->imgbbKeyEdited && buf[0] == L'\0' && !g_cfg.imgbbKey.empty()) {
                    g_st->suppressKeyEvent = true;
                    SetWindowTextW(ek, kMaskedKey);
                    g_st->suppressKeyEvent = false;
                } else if (g_st->imgbbKeyEdited) {
                    CommitRealtimeSave(true);
                    InvalidateRect(ek, NULL, FALSE);
                }
            }
            return 0;
        }
        // 翻译厂家 Base URL（600 + i * 10）与 Model（601 + i * 10）实时同步
        if (g_st && !g_st->suppressKeyEvent && id >= 600 &&
            ((id - 600) % 10 == 0 || (id - 601) % 10 == 0)) {
            if (code == EN_CHANGE) {
                ScheduleRealtimeSave();
            } else if (code == EN_KILLFOCUS && g_st->savePending) {
                CommitRealtimeSave(false);
            }
            return 0;
        }
        // API Key 掩码交互（id = 602 + i * 10）：
        // 点击聚焦时自动清空 ********** 方便直接输入/粘贴新 Key；输入时实时生效；失焦恢复掩码
        if (g_st && !g_st->suppressKeyEvent && id >= 602 && (id - 602) % 10 == 0) {
            size_t i = (size_t)((id - 602) / 10);
            if (i < g_cfg.providers.size() && i < g_st->hKey.size() && g_st->hKey[i]) {
                HWND ek = g_st->hKey[i];
                if (code == EN_SETFOCUS) {
                    if (i < g_st->keyEdited.size() && !g_st->keyEdited[i] &&
                        !g_cfg.providers[i].key.empty()) {
                        g_st->suppressKeyEvent = true;
                        SetWindowTextW(ek, L"");
                        g_st->suppressKeyEvent = false;
                    }
                } else if (code == EN_CHANGE) {
                    if (i < g_st->keyEdited.size()) g_st->keyEdited[i] = true;
                    InvalidateRect(ek, NULL, FALSE);
                    ScheduleRealtimeSave();
                } else if (code == EN_KILLFOCUS) {
                    wchar_t buf[2048] = {};
                    GetWindowTextW(ek, buf, 2048);
                    if (i < g_st->keyEdited.size() && !g_st->keyEdited[i] && buf[0] == L'\0' &&
                        !g_cfg.providers[i].key.empty()) {
                        g_st->suppressKeyEvent = true;
                        SetWindowTextW(ek, kMaskedKey);
                        g_st->suppressKeyEvent = false;
                    } else if (i < g_st->keyEdited.size() && g_st->keyEdited[i]) {
                        CommitRealtimeSave(true);
                        InvalidateRect(ek, NULL, FALSE);
                    }
                }
            }
        }
        return 0;
    }
    case WM_SETCURSOR: {
        if (LOWORD(lp) != HTCLIENT) break;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        SetCursor(LoadCursorW(NULL, HitTest(pt) != ID_NONE ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_MOUSEWHEEL: {
        if (g_st->scrollMax > 0) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int step = MulDiv(S(64), delta, WHEEL_DELTA);
            int next = g_st->scroll - step;
            if (next < 0) next = 0;
            if (next > g_st->scrollMax) next = g_st->scrollMax;
            if (next != g_st->scroll) {
                g_st->scroll = next;
                ComputeLayout(true);
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    }
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_MOUSEMOVE: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g_st->winDragging) {
            POINT sp{};
            GetCursorPos(&sp);
            int curScale = MonitorScaleOf(sp);
            if (curScale != g_st->scale && IsPointInsideMonitorWithMargin(sp, 6)) {
                RECT work = MonitorWorkRect(sp);
                g_st->inSizing = true;
                g_st->scale = curScale;
                RebuildFonts();
                int w = 0, h = 0;
                ComputeDesiredWindowSize(curScale, work, w, h);
                g_st->dragOffX = (int)std::lround(g_st->dragRx * w);
                g_st->dragOffY = (int)std::lround(g_st->dragRy * h);
                int nx = sp.x - g_st->dragOffX;
                int ny = sp.y - g_st->dragOffY;
                SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, w, h, SWP_NOACTIVATE);
                RECT cr{};
                GetClientRect(hwnd, &cr);
                g_st->width = cr.right;
                g_st->height = cr.bottom;
                ComputeLayout(true);
                g_st->inSizing = false;
                InvalidateRect(hwnd, NULL, FALSE);
                UpdateWindow(hwnd);
                ZLog(L"[set] drag cross-monitor scale=%d pos=%d,%d size=%dx%d", curScale, nx, ny, w, h);
            } else {
                SetWindowPos(hwnd, HWND_TOPMOST, sp.x - g_st->dragOffX, sp.y - g_st->dragOffY, 0, 0,
                             SWP_NOSIZE | SWP_NOACTIVATE);
            }
            return 0;
        }
        if (g_st->scrollDrag && g_st->scrollMax > 0) {
            int trackH = (g_st->rcScrollTrack.bottom - g_st->rcScrollTrack.top) -
                         (g_st->rcScrollThumb.bottom - g_st->rcScrollThumb.top);
            if (trackH > 0) {
                int dy = pt.y - g_st->scrollDragStartY;
                int next = g_st->scrollDragStartVal + MulDiv(dy, g_st->scrollMax, trackH);
                if (next < 0) next = 0;
                if (next > g_st->scrollMax) next = g_st->scrollMax;
                if (next != g_st->scroll) {
                    g_st->scroll = next;
                    ComputeLayout(true);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        int hit = HitTest(pt);
        if (hit != g_st->hover) {
            g_st->hover = hit;
            KickAnim();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (!g_st->tracking) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            g_st->tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_st->tracking = false;
        g_st->hover = ID_NONE;
        KickAnim();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = HitTest(pt);
        if (pt.y < S(kTitleH) && hit == ID_NONE) {
            if (g_st->capture >= 0) {
                g_st->capture = -1;
                ApplyHotkeys();
            }
            SetFocus(hwnd);
            POINT sp{};
            GetCursorPos(&sp);
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            int ww = (wr.right - wr.left) > 0 ? (wr.right - wr.left) : 1;
            int wh = (wr.bottom - wr.top) > 0 ? (wr.bottom - wr.top) : 1;
            g_st->winDragging = true;
            g_st->dragOffX = sp.x - wr.left;
            g_st->dragOffY = sp.y - wr.top;
            g_st->dragRx = (double)g_st->dragOffX / (double)ww;
            g_st->dragRy = (double)g_st->dragOffY / (double)wh;
            if (g_st->dragRx < 0.0) g_st->dragRx = 0.0;
            if (g_st->dragRx > 1.0) g_st->dragRx = 1.0;
            if (g_st->dragRy < 0.0) g_st->dragRy = 0.0;
            if (g_st->dragRy > 1.0) g_st->dragRy = 1.0;
            SetCapture(hwnd);
            return 0;
        }
        g_st->pressed = hit;
        if (hit == ID_SCROLLBAR && g_st->scrollMax > 0) {
            int thumbH = g_st->rcScrollThumb.bottom - g_st->rcScrollThumb.top;
            int trackSpan = (g_st->rcScrollTrack.bottom - g_st->rcScrollTrack.top) - thumbH;
            if (!PtIn(g_st->rcScrollThumb, pt) && trackSpan > 0) {
                int targetTop = pt.y - thumbH / 2 - g_st->rcScrollTrack.top;
                int next = MulDiv(targetTop, g_st->scrollMax, trackSpan);
                if (next < 0) next = 0;
                if (next > g_st->scrollMax) next = g_st->scrollMax;
                g_st->scroll = next;
                ComputeLayout(true);
            }
            g_st->scrollDrag = true;
            g_st->scrollDragStartY = pt.y;
            g_st->scrollDragStartVal = g_st->scroll;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (hit >= ID_HOTKEY0 && hit < ID_HOTKEY0 + kHotCount) {
            g_st->capture = hit - ID_HOTKEY0;
            // 录制期间才注销，免得按到 Ctrl+Alt+A 直接把截图拉起来
            if (g_msgWnd) UnregisterHotKey(g_msgWnd, 1);
            SetFocus(hwnd);
        } else if (g_st->tab == TAB_SAVE && PtIn(g_st->rcSaveDirBox, pt) && g_st->hSaveDir) {
            SetFocus(g_st->hSaveDir);
        } else if (g_st->tab == TAB_IMGSEARCH && PtIn(g_st->rcImgbbKey, pt) && g_st->hImgbbKey) {
            SetFocus(g_st->hImgbbKey);
        } else if (g_st->capture >= 0) {
            g_st->capture = -1;
            ApplyHotkeys();
        }
        KickAnim();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_st->winDragging) {
            g_st->winDragging = false;
            g_st->pressed = ID_NONE;
            ReleaseCapture();
            // 松手后：若窗口恰好跨在缝隙且主体落在另一块屏上，做一次最终 DPI 对齐
            RECT wr{};
            if (GetWindowRect(hwnd, &wr) && wr.right > wr.left) {
                POINT mid{(wr.left + wr.right) / 2, (wr.top + wr.bottom) / 2};
                int settleScale = MonitorScaleOf(mid);
                if (settleScale != g_st->scale) {
                    RECT work = MonitorWorkRect(mid);
                    g_st->inSizing = true;
                    g_st->scale = settleScale;
                    RebuildFonts();
                    int w = 0, h = 0;
                    ComputeDesiredWindowSize(settleScale, work, w, h);
                    int nx = mid.x - w / 2;
                    int ny = wr.top;
                    SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, w, h, SWP_NOACTIVATE);
                    RECT cr{};
                    GetClientRect(hwnd, &cr);
                    g_st->width = cr.right;
                    g_st->height = cr.bottom;
                    ComputeLayout(true);
                    g_st->inSizing = false;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;
        }
        if (g_st->scrollDrag) {
            g_st->scrollDrag = false;
            g_st->pressed = ID_NONE;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int hit = HitTest(pt);
        int pressed = g_st->pressed;
        g_st->pressed = ID_NONE;
        InvalidateRect(hwnd, NULL, FALSE);
        if (hit != pressed || hit == ID_NONE) return 0;

        if (hit == ID_CLOSE) {
            CloseSettingsWindow();
            return 0;
        }
        if (hit == ID_RESET) {
            ResetAll();
            return 0;
        }
        if (hit >= ID_TAB0 && hit < ID_TAB0 + kTabCount) {
            CommitRealtimeSave(true);
            SwitchTab(hit - ID_TAB0);
            return 0;
        }
        if (hit == ID_PICK_DIR) {
            PullFieldText(false);
            std::wstring cur = EffectiveSaveRootDir(g_cfg.saveDir);
            std::wstring picked;
            // 临时取消置顶，确保系统文件夹选择框始终显示在最前
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            bool ok = PickFolderDialog(hwnd, cur, picked);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(hwnd);
            if (ok) {
                g_cfg.saveDir = picked;
                if (g_st->hSaveDir) {
                    g_st->suppressKeyEvent = true;
                    SetWindowTextW(g_st->hSaveDir, picked.c_str());
                    g_st->suppressKeyEvent = false;
                }
                CommitRealtimeSave(false);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (hit == ID_OPEN_DIR) {
            PullFieldText(false);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            OpenFolderInExplorer(EffectiveSaveRootDir(g_cfg.saveDir));
            return 0;
        }
        if (hit == ID_TOGGLE_AUTOSAVE) {
            g_cfg.autoSaveShot = !g_cfg.autoSaveShot;
            CommitRealtimeSave(false);
            KickAnim();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (hit == ID_TOGGLE_OPENFOLDER) {
            g_cfg.openFolderAfterSave = !g_cfg.openFolderAfterSave;
            CommitRealtimeSave(false);
            KickAnim();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (hit == ID_TOGGLE_SELECT_MASTER) {
            g_cfg.enableSelectTranslate = !g_cfg.enableSelectTranslate;
            if (!g_cfg.enableSelectTranslate) HideSelectBar();
            CommitRealtimeSave(false);
            KickAnim();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (hit == ID_LANG_FOREIGN || hit == ID_LANG_CHINESE) {
            const RECT& anchor = (hit == ID_LANG_FOREIGN) ? g_st->rcLangForeign : g_st->rcLangChinese;
            std::wstring& cur = (hit == ID_LANG_FOREIGN) ? g_cfg.targetLang : g_cfg.cnTargetLang;
            const auto& langs = SupportedTargetLangs();
            std::vector<ui::DropdownItem> items;
            for (size_t i = 0; i < langs.size(); ++i) {
                ui::DropdownItem it;
                it.id = 1001 + (int)i;
                it.label = langs[i].label;
                it.checked = (_wcsicmp(cur.c_str(), langs[i].code) == 0);
                items.push_back(std::move(it));
            }
            POINT p0{anchor.left, anchor.top};
            POINT p1{anchor.right, anchor.bottom};
            ClientToScreen(hwnd, &p0);
            ClientToScreen(hwnd, &p1);
            RECT anchorScreen{p0.x, p0.y, p1.x, p1.y};
            RECT winScreen{};
            GetWindowRect(hwnd, &winScreen);
            int cmd = ui::ShowDropdownPopup(hwnd, anchorScreen, items, g_st->scale, &winScreen);
            if (cmd >= 1001 && (size_t)(cmd - 1001) < langs.size()) {
                cur = langs[cmd - 1001].code;
                CommitRealtimeSave(false);
                if (g_st) InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (hit == ID_ADD) {
            PullFieldText(true);
            Provider p = MakeProvider(TrKind::OpenAI);
            p.name = L"自定义翻译服务";
            g_cfg.providers.push_back(p);
            SaveSettings();
            CreateFieldEdits();
            UpdateWindowSize(PlaceKeepTopLeft);
            if (g_st->scrollMax > 0) {
                g_st->scroll = g_st->scrollMax;
                ComputeLayout(true);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (hit >= ID_DEL0 && hit < ID_DEL0 + 64) {
            int i = hit - ID_DEL0;
            if (i < (int)g_cfg.providers.size()) {
                if (IsBuiltInProvider(g_cfg.providers[i].kind)) {
                    ShowToast(L"内置默认翻译服务不可删除");
                    return 0;
                }
                if (g_cfg.providers.size() <= 1) {
                    ShowToast(L"至少需要保留一个翻译服务");
                    return 0;
                }
                PullFieldText(true);
                g_cfg.providers.erase(g_cfg.providers.begin() + i);
                EnsureProviderDefaults(g_cfg.providers);
                SaveSettings();
                CreateFieldEdits();
                UpdateWindowSize(PlaceKeepTopLeft);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (hit >= ID_GETKEY0 && hit < ID_GETKEY0 + 64) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShellExecuteW(NULL, L"open", kZhipuKeyUrl, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (hit == ID_GETKEY_IMGBB) {
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ShellExecuteW(NULL, L"open", kImgbbKeyUrl, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (hit >= ID_IMGENGINE0 && hit < ID_IMGENGINE0 + IMG_ENGINE_COUNT) {
            g_cfg.imageSearchEngine = hit - ID_IMGENGINE0;
            CommitRealtimeSave(false);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (hit >= ID_SELECT0 && hit < ID_SELECT0 + 64) {
            int i = hit - ID_SELECT0;
            if (i < (int)g_cfg.providers.size()) {
                if (!g_cfg.providers[i].selectEnabled &&
                    g_cfg.providers[i].kind == TrKind::Zhipu && g_cfg.providers[i].key.empty()) {
                    ShowToast(L"当前智谱AI没有 Key，请先填写 API Key（可点击免费获取）");
                    if (i < g_st->hKey.size() && g_st->hKey[i]) SetFocus(g_st->hKey[i]);
                    return 0;
                }
                if (g_cfg.providers[i].selectEnabled) {
                    int selCnt = 0;
                    for (const auto& p : g_cfg.providers)
                        if (p.selectEnabled) ++selCnt;
                    if (selCnt <= 1) {
                        ShowToast(L"划词翻译至少需要保留一个启用的服务");
                        return 0;
                    }
                }
                g_cfg.providers[i].selectEnabled = !g_cfg.providers[i].selectEnabled;
                CommitRealtimeSave(false);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (hit >= ID_TOGGLE0 && hit < ID_TOGGLE0 + 64) {
            int i = hit - ID_TOGGLE0;
            if (i < (int)g_cfg.providers.size()) {
                if (!g_cfg.providers[i].enabled &&
                    g_cfg.providers[i].kind == TrKind::Zhipu && g_cfg.providers[i].key.empty()) {
                    ShowToast(L"当前智谱AI没有 Key，请先填写 API Key（可点击免费获取）");
                    if (i < g_st->hKey.size() && g_st->hKey[i]) SetFocus(g_st->hKey[i]);
                    return 0;
                }
                if (g_cfg.providers[i].enabled) {
                    int enabledCnt = 0;
                    for (const auto& p : g_cfg.providers)
                        if (p.enabled) ++enabledCnt;
                    if (enabledCnt <= 1) {
                        ShowToast(L"至少需要保留一个启用的翻译服务");
                        return 0;
                    }
                }
                g_cfg.providers[i].enabled = !g_cfg.providers[i].enabled;
                CommitRealtimeSave(false);
                KickAnim();
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN: {
        if (msg == WM_SYSKEYDOWN && (!g_st || g_st->capture < 0)) {
            break;
        }
        if (g_st->capture >= 0) {
            UINT vk = (UINT)wp;
            if (vk == VK_ESCAPE) {
                g_st->capture = -1;
                ApplyHotkeys();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (vk == VK_TAB) {
                g_st->capture = (g_st->capture + 1) % kHotCount;
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            bool modOnly = (vk == VK_CONTROL || vk == VK_MENU || vk == VK_SHIFT || vk == VK_LWIN ||
                            vk == VK_RWIN || vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL);
            if (modOnly) return 0;
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
            if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) mods |= MOD_WIN;
            std::wstring s = FormatHotkey(mods, vk);
            if (!s.empty()) {
                if (std::wstring* v = HotPtr(g_st->capture)) *v = s;
                g_st->capture = -1;
                ApplyHotkeys();
                CommitRealtimeSave(false);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (wp == VK_ESCAPE || wp == VK_RETURN) {
            CloseSettingsWindow();
            return 0;
        }
        if (wp == VK_TAB || wp == VK_RIGHT) {
            CommitRealtimeSave(true);
            SwitchTab((g_st->tab + 1) % kTabCount);
            return 0;
        }
        if (wp == VK_LEFT) {
            CommitRealtimeSave(true);
            SwitchTab((g_st->tab + kTabCount - 1) % kTabCount);
            return 0;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerAnim) {
            UpdateAnim();
        } else if (wp == kTimerAutoSave) {
            CommitRealtimeSave(false);
        }
        return 0;
    case WM_CLOSE:
        CloseSettingsWindow();
        return 0;
    case WM_DESTROY: {
        if (g_st && g_st->savePending) {
            KillTimer(hwnd, kTimerAutoSave);
            g_st->savePending = false;
            PullFieldText(false);
            EnsureProviderDefaults(g_cfg.providers);
            SaveSettings();
        }
        DestroyFieldEdits();
        if (g_st->fTitle) DeleteObject(g_st->fTitle);
        if (g_st->fSub) DeleteObject(g_st->fSub);
        if (g_st->fSection) DeleteObject(g_st->fSection);
        if (g_st->fLabel) DeleteObject(g_st->fLabel);
        if (g_st->fSmall) DeleteObject(g_st->fSmall);
        if (g_st->fField) DeleteObject(g_st->fField);
        if (g_st->fBtn) DeleteObject(g_st->fBtn);
        if (g_st->fTab) DeleteObject(g_st->fTab);
        if (g_st->brushField) DeleteObject(g_st->brushField);
        if (g_st->icon && g_st->iconOwned) DestroyIcon(g_st->icon);
        delete g_st;
        g_st = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool IsSettingsOpen() { return g_st != nullptr; }

void ShowSettingsWindow(HWND owner, int initialTab) {
    static bool cls = false;
    if (!cls) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"ZpixSettings";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        cls = true;
    }
    if (g_st) {
        if (initialTab >= 0 && initialTab < kTabCount) SwitchTab(initialTab);
        ShowWindow(g_st->hwnd, SW_SHOW);
        SetForegroundWindow(g_st->hwnd);
        if (initialTab == TAB_IMGSEARCH && g_cfg.imgbbKey.empty() && g_st->hImgbbKey) {
            SetFocus(g_st->hImgbbKey);
        }
        return;
    }
    g_pendingInitialTab = initialTab;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"ZpixSettings", L"Zpix 设置",
                                WS_POPUP | WS_CLIPCHILDREN, 0, 0, 480, 320, owner, NULL,
                                GetModuleHandleW(NULL), NULL);
    if (!hwnd) return;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    if (initialTab == TAB_IMGSEARCH && g_cfg.imgbbKey.empty() && g_st && g_st->hImgbbKey) {
        SetFocus(g_st->hImgbbKey);
    }
}