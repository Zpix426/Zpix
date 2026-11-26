#include "img_translate.h"
#include "config.h"
#include "ocr_paddle.h"
#include "translate.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace img_translate {
namespace {

struct Rgb {
    int r = 255, g = 255, b = 255;
};

inline Rgb UnpackRgb(uint32_t px) {
    return Rgb{(int)((px >> 16) & 0xFF), (int)((px >> 8) & 0xFF), (int)(px & 0xFF)};
}

inline uint32_t PackRgb(int r, int g, int b) {
    r = std::max(0, std::min(255, r));
    g = std::max(0, std::min(255, g));
    b = std::max(0, std::min(255, b));
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

inline int Luminance(const Rgb& c) {
    return (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
}

inline int ColorDistSq(const Rgb& a, const Rgb& b) {
    int dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

inline int Chroma(const Rgb& c) {
    int mx = std::max(c.r, std::max(c.g, c.b));
    int mn = std::min(c.r, std::min(c.g, c.b));
    return mx - mn;
}

static int MedianOf(std::vector<int>& v, int fallback = 255) {
    if (v.empty()) return fallback;
    size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

// ---------------------------------------------------------------- 语种与可翻译块筛选

struct BlockScript {
    int han = 0;
    int foreign = 0;  // 拉丁 / 假名 / 韩文 / 西里尔
    int digits = 0;
};

static BlockScript InspectBlockScript(const std::wstring& s) {
    BlockScript bs;
    for (wchar_t ch : s) {
        if ((ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0x3400 && ch <= 0x4DBF) ||
            (ch >= 0xF900 && ch <= 0xFAFF)) {
            ++bs.han;
        } else if (((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')) ||
                   (ch >= 0x00C0 && ch <= 0x024F) || (ch >= 0x1E00 && ch <= 0x1EFF) ||
                   (ch >= 0x3040 && ch <= 0x30FF) || (ch >= 0x31F0 && ch <= 0x31FF) ||
                   (ch >= 0xAC00 && ch <= 0xD7AF) || (ch >= 0x0400 && ch <= 0x04FF)) {
            ++bs.foreign;
        } else if (ch >= L'0' && ch <= L'9') {
            ++bs.digits;
        }
    }
    return bs;
}

static bool IsUrlOrCodeTokenOnly(const std::wstring& s) {
    std::wstring t;
    for (wchar_t c : s) {
        if (!iswspace(c)) t.push_back(towlower(c));
    }
    if (t.compare(0, 7, L"http://") == 0 || t.compare(0, 8, L"https://") == 0 ||
        t.compare(0, 4, L"www.") == 0) {
        return s.find(L' ') == std::wstring::npos;
    }
    return false;
}

// ---------------------------------------------------------------- 原文字前景色/背景色与粗体特征分析

struct StyleSample {
    Rgb bg{255, 255, 255};
    Rgb fg{28, 30, 34};
    int bgSpread = 0;       // 背景杂色离散度（四分位距，越大说明越像复杂照片底）
    bool boldText = false;  // 原文是否偏粗体（标题/按钮）
};

static bool PointInsideAnyRect(int x, int y, const std::vector<RECT>& rcs) {
    for (const RECT& r : rcs) {
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
    }
    return false;
}

static StyleSample AnalyzeBlockStyle(const Bitmap32& img, const ocr_paddle::OcrBlock& blk) {
    StyleSample st;
    std::vector<int> rs, gs, bs, lums;

    // 采集每行文字框外围 1~4px 环带像素作为真实背景样本（排除落入其他文字行内的像素）
    for (const RECT& lr : blk.lineRcs) {
        int x0 = std::max(0, (int)lr.left - 4);
        int y0 = std::max(0, (int)lr.top - 4);
        int x1 = std::min(img.w, (int)lr.right + 4);
        int y1 = std::min(img.h, (int)lr.bottom + 4);
        for (int y = y0; y < y1; ++y) {
            const uint32_t* row = img.row(y);
            for (int x = x0; x < x1; ++x) {
                if (x >= lr.left && x < lr.right && y >= lr.top && y < lr.bottom) continue;
                if (PointInsideAnyRect(x, y, blk.lineRcs)) continue;
                Rgb c = UnpackRgb(row[x]);
                rs.push_back(c.r);
                gs.push_back(c.g);
                bs.push_back(c.b);
                lums.push_back(Luminance(c));
            }
        }
    }

    // 贴边极端情况：退回文字框四边最外 1px 采样
    if (rs.empty()) {
        RECT r = blk.rc;
        for (int x = r.left; x < r.right && x < img.w; ++x) {
            if (r.top >= 0 && r.top < img.h) {
                Rgb c = UnpackRgb(img.row(r.top)[x]);
                rs.push_back(c.r); gs.push_back(c.g); bs.push_back(c.b); lums.push_back(Luminance(c));
            }
            if (r.bottom - 1 >= 0 && r.bottom - 1 < img.h) {
                Rgb c = UnpackRgb(img.row(r.bottom - 1)[x]);
                rs.push_back(c.r); gs.push_back(c.g); bs.push_back(c.b); lums.push_back(Luminance(c));
            }
        }
    }

    st.bg.r = MedianOf(rs, 255);
    st.bg.g = MedianOf(gs, 255);
    st.bg.b = MedianOf(bs, 255);

    if (lums.size() >= 8) {
        std::sort(lums.begin(), lums.end());
        int p15 = lums[lums.size() * 15 / 100];
        int p85 = lums[lums.size() * 85 / 100];
        st.bgSpread = p85 - p15;
    }

    // 在行框内部统计与背景对比度最高的像素，提取原文字前景色及笔画浓度
    struct PxDist {
        int distSq;
        Rgb c;
    };
    std::vector<PxDist> inner;
    int totalInner = 0;
    int inkPixels = 0;
    for (const RECT& lr : blk.lineRcs) {
        int x0 = std::max(0, (int)lr.left);
        int y0 = std::max(0, (int)lr.top);
        int x1 = std::min(img.w, (int)lr.right);
        int y1 = std::min(img.h, (int)lr.bottom);
        for (int y = y0; y < y1; ++y) {
            const uint32_t* row = img.row(y);
            for (int x = x0; x < x1; ++x) {
                Rgb c = UnpackRgb(row[x]);
                int d2 = ColorDistSq(c, st.bg);
                ++totalInner;
                if (d2 >= 42 * 42) {
                    ++inkPixels;
                    inner.push_back(PxDist{d2, c});
                }
            }
        }
    }

    if (totalInner > 0) {
        double inkRatio = (double)inkPixels / (double)totalInner;
        // 笔画墨迹占行框比例较高（或大字号短标题）用半粗体，普通正文保持清爽常规字重
        st.boldText = (inkRatio > 0.33) || (blk.avgLineH >= 28.0f && inkRatio > 0.24);
    }

    int bgLum = Luminance(st.bg);
    if (!inner.empty()) {
        // 取对比度前 40% 的核心笔画像素（过滤抗锯齿半透明灰边，避免字色发虚变浅）
        std::sort(inner.begin(), inner.end(),
                  [](const PxDist& a, const PxDist& b) { return a.distSq > b.distSq; });
        size_t keepN = std::max<size_t>(1, inner.size() * 2 / 5);
        std::vector<int> fr, fg, fb;
        fr.reserve(keepN); fg.reserve(keepN); fb.reserve(keepN);
        for (size_t i = 0; i < keepN; ++i) {
            fr.push_back(inner[i].c.r);
            fg.push_back(inner[i].c.g);
            fb.push_back(inner[i].c.b);
        }
        st.fg = Rgb{MedianOf(fr, 28), MedianOf(fg, 30), MedianOf(fb, 34)};

        int fgLum = Luminance(st.fg);
        int chr = Chroma(st.fg);
        // 浅底深灰/黑字：拉实黑度，消除英文细笔画抗锯齿带来的发灰感；彩色字（蓝链/红字）保留原色！
        if (bgLum > 205 && fgLum < 115 && chr < 28) {
            st.fg = Rgb{std::min(st.fg.r, 32), std::min(st.fg.g, 34), std::min(st.fg.b, 38)};
        } else if (bgLum < 65 && fgLum > 145 && chr < 28) {
            st.fg = Rgb{std::max(st.fg.r, 236), std::max(st.fg.g, 238), std::max(st.fg.b, 242)};
        } else if (std::abs(fgLum - bgLum) < 65) {
            st.fg = (bgLum > 135) ? Rgb{26, 28, 32} : Rgb{244, 246, 250};
        }
    } else {
        st.fg = (bgLum > 135) ? Rgb{26, 28, 32} : Rgb{244, 246, 250};
    }

    return st;
}

// ---------------------------------------------------------------- 中英文混排 + 避头尾标点折行排版

static bool IsClosingPunct(wchar_t c) {
    static const wchar_t* kSet = L"，。！？；：、）》」』】”’〉》.,!?;:)]}%";
    return wcschr(kSet, c) != nullptr;
}

static bool IsOpeningPunct(wchar_t c) {
    static const wchar_t* kSet = L"（《「『【“‘〈([{";
    return wcschr(kSet, c) != nullptr;
}

static bool IsAsciiWordChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ||
           c == L'\'';
}

static std::vector<std::wstring> WrapTextToWidth(HDC dc, const std::wstring& text, int maxWidthPx) {
    std::vector<std::wstring> out;
    if (text.empty()) return out;
    maxWidthPx = std::max(20, maxWidthPx);

    size_t pos = 0;
    const size_t n = text.size();
    while (pos < n) {
        size_t nl = text.find(L'\n', pos);
        size_t paraEnd = (nl == std::wstring::npos) ? n : nl;

        size_t cur = pos;
        while (cur < paraEnd && text[cur] == L' ') ++cur;
        if (cur == paraEnd) {
            pos = (nl == std::wstring::npos) ? n : (nl + 1);
            continue;
        }

        while (cur < paraEnd) {
            int rem = (int)(paraEnd - cur);
            INT fit = 0;
            SIZE sz{};
            if (!GetTextExtentExPointW(dc, text.c_str() + cur, rem, maxWidthPx, &fit, NULL, &sz) ||
                fit <= 0) {
                fit = 1;
            }
            if (fit >= rem) {
                std::wstring ln = text.substr(cur, (size_t)rem);
                while (!ln.empty() && ln.back() == L' ') ln.pop_back();
                if (!ln.empty()) out.push_back(std::move(ln));
                break;
            }

            size_t brk = (size_t)fit;

            // 英文单词不断词：若截断点两侧都是英文字母，回退到最近的空格/连字符处折行
            if (IsAsciiWordChar(text[cur + brk - 1]) && IsAsciiWordChar(text[cur + brk])) {
                size_t scan = brk;
                while (scan > 1 && brk - scan <= 20) {
                    wchar_t prev = text[cur + scan - 1];
                    if (prev == L' ' || prev == L'-' || prev == L'/') {
                        brk = scan;
                        break;
                    }
                    --scan;
                }
            }

            // 避头标点：禁止句号/逗号/右括号出现在下一行行首
            if (cur + brk < paraEnd && IsClosingPunct(text[cur + brk])) {
                if (cur + brk + 1 <= paraEnd) {
                    ++brk;
                }
            } else if (brk > 2 && IsOpeningPunct(text[cur + brk - 1])) {
                // 避尾标点：禁止左括号/前引号孤立在行尾
                --brk;
            }

            std::wstring ln = text.substr(cur, brk);
            while (!ln.empty() && ln.back() == L' ') ln.pop_back();
            if (!ln.empty()) out.push_back(std::move(ln));

            cur += brk;
            while (cur < paraEnd && text[cur] == L' ') ++cur;
        }

        pos = (nl == std::wstring::npos) ? n : (nl + 1);
    }
    return out;
}

// ---------------------------------------------------------------- 段落排版计划与背景无痕擦除

struct BlockPlan {
    size_t blockIdx = 0;
    StyleSample style;
    int fontPx = 16;
    int tmHeight = 20;
    int lineStep = 22;
    int drawX = 0;
    int drawY = 0;
    int wrapW = 100;
    RECT paintBounds{};  // 译文实际占用的矩形区域
    std::vector<std::wstring> lines;
};

static HFONT CreateTransFont(int fontPx, bool bold) {
    return CreateFontW(-fontPx, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

static BlockPlan BuildBlockPlan(HDC measureDc, const Bitmap32& img,
                                const std::vector<ocr_paddle::OcrBlock>& allBlocks, size_t idx,
                                const std::wstring& trText, const StyleSample& style) {
    const auto& blk = allBlocks[idx];
    BlockPlan plan;
    plan.blockIdx = idx;
    plan.style = style;

    RECT rc = blk.rc;
    int origW = std::max<int>(16, rc.right - rc.left);
    int origH = std::max<int>(12, rc.bottom - rc.top);

    // 右侧可用宽度上限：保持原段落右边距，若右侧紧邻其他列则严格避让
    int rightLimit = std::min(img.w - 2, (int)rc.right + 6);
    for (size_t j = 0; j < allBlocks.size(); ++j) {
        if (j == idx) continue;
        const RECT& o = allBlocks[j].rc;
        if (o.left >= rc.right - 2 && o.bottom > rc.top + 4 && o.top < rc.bottom - 4) {
            rightLimit = std::min<int>(rightLimit, o.left - 6);
        }
    }
    // 下方可用高度上限：可轻微利用段后留白，但绝不压到下一段文字顶边
    int bottomLimit = std::min(img.h - 2, (int)rc.bottom + (int)std::lround(blk.avgLineH * 0.42f));
    for (size_t j = 0; j < allBlocks.size(); ++j) {
        if (j == idx) continue;
        const RECT& o = allBlocks[j].rc;
        if (o.top >= rc.bottom - 2 && o.right > rc.left + 4 && o.left < rc.right - 4) {
            bottomLimit = std::min<int>(bottomLimit, o.top - 4);
        }
    }
    int maxAvailH = std::max(origH, bottomLimit - (int)rc.top);

    plan.drawX = std::max(1, (int)rc.left + 1);
    plan.wrapW = std::max(24, rightLimit - plan.drawX);

    // PP-OCRv5 unclip=1.5 框高约为实际字号的 1.25~1.30 倍，乘 0.78 精准还原原始视觉字号
    int initFontPx = (int)std::lround(blk.avgLineH * 0.78f);
    initFontPx = std::max(12, std::min(56, initFontPx));
    const int minFontPx = 11;

    float origPitch = 0.0f;
    if (blk.lineRcs.size() >= 2) {
        origPitch = (float)(blk.lineRcs.back().top - blk.lineRcs.front().top) /
                    (float)(blk.lineRcs.size() - 1);
    }

    for (int fpx = initFontPx; fpx >= minFontPx; --fpx) {
        HFONT hf = CreateTransFont(fpx, style.boldText);
        HGDIOBJ oldF = SelectObject(measureDc, hf);
        TEXTMETRICW tm{};
        GetTextMetricsW(measureDc, &tm);

        std::vector<std::wstring> wrapped = WrapTextToWidth(measureDc, trText, plan.wrapW);
        SelectObject(measureDc, oldF);
        DeleteObject(hf);

        if (wrapped.empty()) break;

        int step = (int)std::lround(fpx * 1.38f);
        if (origPitch > 0.0f && wrapped.size() <= blk.lineRcs.size()) {
            // 译文行数不多于原文时，直接继承原段落的舒适行距
            step = std::max((int)std::lround(fpx * 1.30f),
                            std::min((int)std::lround(fpx * 1.72f), (int)std::lround(origPitch)));
        } else if (wrapped.size() > blk.lineRcs.size()) {
            step = std::max((int)tm.tmHeight, (int)std::lround(fpx * 1.26f));
        }
        step = std::max((int)tm.tmHeight, step);

        int totalH = (int)(wrapped.size() - 1) * step + tm.tmHeight;
        plan.fontPx = fpx;
        plan.tmHeight = tm.tmHeight;
        plan.lineStep = step;
        plan.lines = std::move(wrapped);

        if (totalH <= maxAvailH || fpx == minFontPx) {
            break;
        }
    }

    // 计算每一行实际绘制宽度与垂直起点（单行按钮/标题垂直居中，多行自然段顶部对齐）
    int actualMaxW = 0;
    if (!plan.lines.empty()) {
        HFONT hf = CreateTransFont(plan.fontPx, style.boldText);
        HGDIOBJ oldF = SelectObject(measureDc, hf);
        for (const auto& ln : plan.lines) {
            SIZE sz{};
            GetTextExtentPoint32W(measureDc, ln.c_str(), (int)ln.size(), &sz);
            actualMaxW = std::max<int>(actualMaxW, sz.cx);
        }
        SelectObject(measureDc, oldF);
        DeleteObject(hf);
    }

    int totalH = plan.lines.empty() ? 0 : ((int)(plan.lines.size() - 1) * plan.lineStep + plan.tmHeight);
    if (blk.lineCount == 1 && plan.lines.size() == 1) {
        plan.drawY = rc.top + (origH - plan.tmHeight) / 2;
    } else {
        plan.drawY = rc.top + 1;
    }
    plan.drawY = std::max(0, std::min(std::max(0, img.h - totalH), plan.drawY));

    plan.paintBounds = RECT{
        plan.drawX,
        plan.drawY,
        std::min(img.w, plan.drawX + actualMaxW + 2),
        std::min(img.h, plan.drawY + totalH + 1),
    };
    return plan;
}

// 在纯色/微渐变背景上按左右边界平滑插值擦除单个矩形（自动剔除邻近图标/边框异色干扰，边缘 1px 羽化）
static void SmoothEraseRect(Bitmap32& dst, const Bitmap32& orig, RECT r, const Rgb& bgMed) {
    int x0 = std::max(0, (int)r.left - 2);
    int y0 = std::max(0, (int)r.top - 1);
    int x1 = std::min(dst.w, (int)r.right + 2);
    int y1 = std::min(dst.h, (int)r.bottom + 1);
    if (x1 - x0 < 2 || y1 - y0 < 2) return;

    auto safeBoundarySample = [&](int sx, int sy) -> Rgb {
        sx = std::max(0, std::min(orig.w - 1, sx));
        sy = std::max(0, std::min(orig.h - 1, sy));
        Rgb c = UnpackRgb(orig.row(sy)[sx]);
        // 若边界紧贴图标/分割线导致色差过大，回退到块背景中位色，彻底杜绝拉出彩色条纹
        if (ColorDistSq(c, bgMed) > 36 * 36) return bgMed;
        return c;
    };

    int w = x1 - x0;
    for (int y = y0; y < y1; ++y) {
        Rgb leftCol = safeBoundarySample(x0 - 1, y);
        Rgb rightCol = safeBoundarySample(x1, y);
        bool borderY = (y == y0 && y0 > 0) || (y == y1 - 1 && y1 < dst.h);

        uint32_t* outRow = dst.row(y);
        const uint32_t* srcRow = orig.row(y);
        for (int x = x0; x < x1; ++x) {
            double t = (w > 1) ? (double)(x - x0) / (double)(w - 1) : 0.0;
            int er = (int)std::lround(leftCol.r + (rightCol.r - leftCol.r) * t);
            int eg = (int)std::lround(leftCol.g + (rightCol.g - leftCol.g) * t);
            int eb = (int)std::lround(leftCol.b + (rightCol.b - leftCol.b) * t);

            bool borderX = (x == x0 && x0 > 0) || (x == x1 - 1 && x1 < dst.w);
            if (borderX || borderY) {
                Rgb o = UnpackRgb(srcRow[x]);
                // 若边缘原本不是深色笔画，做 50% 柔化过渡消除硬框感
                if (ColorDistSq(o, bgMed) < 48 * 48) {
                    er = (er + o.r) / 2;
                    eg = (eg + o.g) / 2;
                    eb = (eb + o.b) / 2;
                }
            }
            outRow[x] = PackRgb(er, eg, eb);
        }
    }
}

// 复杂花纹/照片底兜底：先柔化模糊原笔画，再铺一层圆角半透明柔和衬底保证译文清晰
static void BlurAndBackdropComplexBlock(Bitmap32& dst, const Bitmap32& orig,
                                        const ocr_paddle::OcrBlock& blk, const BlockPlan& plan) {
    // 1) 对原文字行做 5x5 均值柔化，弱化高频原字边缘
    for (const RECT& lr : blk.lineRcs) {
        int x0 = std::max(0, (int)lr.left - 1);
        int y0 = std::max(0, (int)lr.top - 1);
        int x1 = std::min(dst.w, (int)lr.right + 1);
        int y1 = std::min(dst.h, (int)lr.bottom + 1);
        for (int y = y0; y < y1; ++y) {
            uint32_t* outRow = dst.row(y);
            for (int x = x0; x < x1; ++x) {
                int sr = 0, sg = 0, sb = 0, cnt = 0;
                for (int dy = -3; dy <= 3; ++dy) {
                    int ny = std::max(0, std::min(orig.h - 1, y + dy));
                    const uint32_t* rptr = orig.row(ny);
                    for (int dx = -3; dx <= 3; ++dx) {
                        int nx = std::max(0, std::min(orig.w - 1, x + dx));
                        Rgb c = UnpackRgb(rptr[nx]);
                        sr += c.r; sg += c.g; sb += c.b;
                        ++cnt;
                    }
                }
                outRow[x] = PackRgb(sr / cnt, sg / cnt, sb / cnt);
            }
        }
    }

    // 2) 在译文区域叠加 82% 不透明度的柔和底色衬板
    RECT box = plan.paintBounds;
    InflateRect(&box, 5, 3);
    int x0 = std::max(0, (int)box.left);
    int y0 = std::max(0, (int)box.top);
    int x1 = std::min(dst.w, (int)box.right);
    int y1 = std::min(dst.h, (int)box.bottom);
    Rgb padCol = plan.style.bg;
    const int alpha = 212;  // 0..255
    for (int y = y0; y < y1; ++y) {
        uint32_t* row = dst.row(y);
        for (int x = x0; x < x1; ++x) {
            Rgb cur = UnpackRgb(row[x]);
            int nr = (padCol.r * alpha + cur.r * (255 - alpha)) / 255;
            int ng = (padCol.g * alpha + cur.g * (255 - alpha)) / 255;
            int nb = (padCol.b * alpha + cur.b * (255 - alpha)) / 255;
            row[x] = PackRgb(nr, ng, nb);
        }
    }
}

}  // namespace

bool TranslateSelectionImage(const Bitmap32& srcSel, Result& out) {
    out = Result{};
    if (srcSel.empty() || srcSel.w < 4 || srcSel.h < 4) {
        out.error = L"选区过小";
        return false;
    }

    // 1) 本地 PP-OCRv5 提取带精确行框的自然段/词块
    std::vector<ocr_paddle::OcrBlock> allBlocks;
    int ocrMs = 0;
    if (!ocr_paddle::RecognizeBlocksBgra((const unsigned char*)srcSel.px.data(), srcSel.w,
                                         srcSel.h, srcSel.w * 4, allBlocks, &ocrMs)) {
        out.error = L"本地 OCR 引擎未就绪";
        return false;
    }
    out.ocrMs = ocrMs;

    if (allBlocks.empty()) {
        out.error = L"未识别到文字";
        return false;
    }

    // 2) 智能筛选需要翻译的块并决定目标语言：
    //    - 只要图内存在外文块（英文/日文/德文/韩文/俄文等），统一译为用户设定的外语目标（默认简体中文），
    //      同时保留图内原本就是纯中文的 UI 标签不重绘；
    //    - 若整张图全是纯中文，则自动译为中文目标语种（默认英语）。
    std::vector<BlockScript> scripts(allBlocks.size());
    int foreignBlockCount = 0;
    int chineseBlockCount = 0;
    for (size_t i = 0; i < allBlocks.size(); ++i) {
        scripts[i] = InspectBlockScript(allBlocks[i].text);
        if (IsUrlOrCodeTokenOnly(allBlocks[i].text)) continue;
        if (scripts[i].foreign >= 2 && scripts[i].han * 2 <= scripts[i].foreign) {
            ++foreignBlockCount;
        } else if (scripts[i].han >= 1) {
            ++chineseBlockCount;
        } else if (scripts[i].foreign >= 1 && scripts[i].han == 0) {
            ++foreignBlockCount;
        }
    }

    if (foreignBlockCount == 0 && chineseBlockCount == 0) {
        out.error = L"未识别到可翻译的文字";
        return false;
    }

    std::wstring targetLang;
    bool translateForeignToCnMode = (foreignBlockCount > 0);
    if (translateForeignToCnMode) {
        targetLang = g_cfg.targetLang.empty() ? L"zh-CN" : g_cfg.targetLang;
        // 防止配置误设为 en 导致英文翻英文无变化
        if (_wcsicmp(targetLang.c_str(), L"en") == 0) targetLang = L"zh-CN";
    } else {
        targetLang = g_cfg.cnTargetLang.empty() ? L"en" : g_cfg.cnTargetLang;
        if (_wcsicmp(targetLang.c_str(), L"zh-CN") == 0 || _wcsicmp(targetLang.c_str(), L"zh") == 0) {
            targetLang = L"en";
        }
    }
    out.targetLang = targetLang;

    std::vector<size_t> activeIdx;
    std::vector<std::wstring> srcTexts;
    for (size_t i = 0; i < allBlocks.size(); ++i) {
        if (IsUrlOrCodeTokenOnly(allBlocks[i].text)) continue;
        const BlockScript& bs = scripts[i];
        if (bs.han == 0 && bs.foreign == 0) continue;
        if (translateForeignToCnMode) {
            // 目标是中文时，已是纯中文的块保持原图清晰像素不动
            bool wantCn = (targetLang.size() >= 2 && (targetLang[0] == L'z' || targetLang[0] == L'Z'));
            if (wantCn && bs.han >= 1 && bs.foreign <= 2) continue;
        }
        activeIdx.push_back(i);
        srcTexts.push_back(allBlocks[i].text);
    }

    if (srcTexts.empty()) {
        out.error = L"图内文字已是目标语言";
        return false;
    }

    // 3) 批量请求翻译
    ULONGLONG tTr0 = GetTickCount64();
    std::vector<std::wstring> dstTexts;
    std::wstring trErr;
    if (!TranslateBatch(srcTexts, dstTexts, targetLang, &out.provider, &trErr)) {
        out.error = trErr.empty() ? L"翻译失败，请稍后重试" : trErr;
        return false;
    }
    out.trMs = (int)(GetTickCount64() - tTr0);

    // 4) 先基于纯净原图采样每个块的颜色风格与排版尺寸，再统一擦除、最后统一绘制译文
    HDC screenDc = GetDC(NULL);
    HDC memDc = CreateCompatibleDC(screenDc);

    std::vector<BlockPlan> plans;
    plans.reserve(activeIdx.size());
    for (size_t k = 0; k < activeIdx.size(); ++k) {
        if (dstTexts[k].empty()) continue;
        size_t bi = activeIdx[k];
        StyleSample style = AnalyzeBlockStyle(srcSel, allBlocks[bi]);
        BlockPlan plan = BuildBlockPlan(memDc, srcSel, allBlocks, bi, dstTexts[k], style);
        if (!plan.lines.empty()) {
            plans.push_back(std::move(plan));
        }
    }

    if (plans.empty()) {
        DeleteDC(memDc);
        ReleaseDC(NULL, screenDc);
        out.error = L"未生成有效译文";
        return false;
    }

    Bitmap32 canvas = srcSel;

    // Pass 1：精准擦除原文字行（不误伤行尾空白处的图标/边框）
    for (const auto& plan : plans) {
        const auto& blk = allBlocks[plan.blockIdx];
        if (plan.style.bgSpread <= 32) {
            for (const RECT& lr : blk.lineRcs) {
                SmoothEraseRect(canvas, srcSel, lr, plan.style.bg);
            }
            // 若译文排版范围略微超出某短行（如最后一行变长），一并平滑擦净底色
            SmoothEraseRect(canvas, srcSel, plan.paintBounds, plan.style.bg);
        } else {
            BlurAndBackdropComplexBlock(canvas, srcSel, blk, plan);
        }
    }

    // Pass 2：在 32bpp DIB 上用 ClearType 抗锯齿逐行渲染译文段落
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = canvas.w;
    bi.bmiHeader.biHeight = -canvas.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (dib && bits) {
        memcpy(bits, canvas.px.data(), canvas.px.size() * 4);
        HGDIOBJ oldBmp = SelectObject(memDc, dib);
        SetBkMode(memDc, TRANSPARENT);

        for (const auto& plan : plans) {
            HFONT hf = CreateTransFont(plan.fontPx, plan.style.boldText);
            HGDIOBJ oldFont = SelectObject(memDc, hf);
            COLORREF fgCol = RGB(plan.style.fg.r, plan.style.fg.g, plan.style.fg.b);
            SetTextColor(memDc, fgCol);

            for (size_t li = 0; li < plan.lines.size(); ++li) {
                int y = plan.drawY + (int)li * plan.lineStep;
                TextOutW(memDc, plan.drawX, y, plan.lines[li].c_str(), (int)plan.lines[li].size());
            }

            SelectObject(memDc, oldFont);
            DeleteObject(hf);
        }

        GdiFlush();
        memcpy(canvas.px.data(), bits, canvas.px.size() * 4);
        SelectObject(memDc, oldBmp);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(memDc);
    ReleaseDC(NULL, screenDc);

    for (auto& px : canvas.px) px |= 0xFF000000u;

    out.image = std::move(canvas);
    out.blockCount = (int)plans.size();
    ZLog(L"[img-tr] ok blocks=%d ocrMs=%d trMs=%d prov=%ls lang=%ls", out.blockCount, out.ocrMs,
         out.trMs, out.provider.c_str(), out.targetLang.c_str());
    return true;
}

bool TranslateImageFile(const std::wstring& inPath, const std::wstring& outPngPath, Result& out) {
    out = Result{};
    InitGdiPlus();
    Gdiplus::Bitmap bmp(inPath.c_str(), FALSE);
    if (bmp.GetLastStatus() != Gdiplus::Ok) {
        out.error = L"无法加载输入图片";
        return false;
    }
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) {
        out.error = L"图片尺寸无效";
        return false;
    }
    Gdiplus::BitmapData bd{};
    Gdiplus::Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok) {
        out.error = L"锁定图片像素失败";
        return false;
    }
    Bitmap32 src;
    src.w = w;
    src.h = h;
    src.px.resize((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = (const uint8_t*)bd.Scan0 + (size_t)y * bd.Stride;
        memcpy(src.row(y), s, (size_t)w * 4);
    }
    bmp.UnlockBits(&bd);

    if (!TranslateSelectionImage(src, out)) return false;
    if (!outPngPath.empty()) {
        if (!SavePngFile(out.image, outPngPath)) {
            out.error = L"写出结果图片失败";
            return false;
        }
    }
    return true;
}

}  // namespace img_translate
