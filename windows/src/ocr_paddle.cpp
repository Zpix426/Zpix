// PP-OCRv5 本地 OCR：ONNXRuntime 推理 + DB 检测后处理 + CTC 解码
// 依赖：onnxruntime.dll、det.onnx、rec.onnx、ppocr_keys.txt（都在 modelDir 里）
// 设计原则：全部自包含，失败就返回空串让上层回退 PowerShell 方案。

#include "ocr_paddle.h"

#include <windows.h>
#include <gdiplus.h>
#include <onnxruntime_c_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace ocr_paddle {
namespace {

// ---------- 常量（对齐 PP-OCRv5 mobile ONNX 官方推理参数）----------
const float kDetMean[3] = {0.485f, 0.456f, 0.406f};
const float kDetStd[3] = {0.229f, 0.224f, 0.225f};
const int kDetLimitSide = 736;      // 短边放大到 736
const int kDetMaxInput = 2000;      // 输入长边上限（长截图的小字要靠它，别降）
const int kDetPadMin = 24;          // 贴边大字必须留白，否则 DB 把行切碎
const float kDetThresh = 0.3f;      // 概率图二值化
const float kDetBoxThresh = 0.5f;   // 框平均分下限
const float kDetUnclip = 1.5f;      // PP-OCRv5 官方默认外扩系数
const int kRecH = 48;
const int kRecMinW = 320;           // rec 输入最小宽度（右侧补归一化后的 0.0）
const int kRecMaxW = 1600;
const int kTopK = 0;

// ---------- 简单图像 ----------
struct Img {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;  // w*h*3
    bool ok() const { return w > 0 && h > 0 && (int)rgb.size() == w * h * 3; }
};

inline unsigned char ClampU8(double v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (unsigned char)(v + 0.5);
}

// 双线性缩放
Img ResizeBilinear(const Img& src, int dw, int dh) {
    Img dst;
    if (!src.ok() || dw <= 0 || dh <= 0) return dst;
    dst.w = dw; dst.h = dh;
    dst.rgb.resize((size_t)dw * dh * 3);
    const double sx = (double)src.w / dw, sy = (double)src.h / dh;
    for (int y = 0; y < dh; ++y) {
        double fy = (y + 0.5) * sy - 0.5;
        int y0 = (int)floor(fy);
        double wy = fy - y0;
        if (y0 < 0) { y0 = 0; wy = 0; }
        int y1 = std::min(y0 + 1, src.h - 1);
        for (int x = 0; x < dw; ++x) {
            double fx = (x + 0.5) * sx - 0.5;
            int x0 = (int)floor(fx);
            double wx = fx - x0;
            if (x0 < 0) { x0 = 0; wx = 0; }
            int x1 = std::min(x0 + 1, src.w - 1);
            const unsigned char* p00 = &src.rgb[((size_t)y0 * src.w + x0) * 3];
            const unsigned char* p01 = &src.rgb[((size_t)y0 * src.w + x1) * 3];
            const unsigned char* p10 = &src.rgb[((size_t)y1 * src.w + x0) * 3];
            const unsigned char* p11 = &src.rgb[((size_t)y1 * src.w + x1) * 3];
            unsigned char* o = &dst.rgb[((size_t)y * dw + x) * 3];
            for (int c = 0; c < 3; ++c) {
                double top = p00[c] + (p01[c] - p00[c]) * wx;
                double bot = p10[c] + (p11[c] - p10[c]) * wx;
                o[c] = ClampU8(top + (bot - top) * wy);
            }
        }
    }
    return dst;
}

// 四角双线性采样：把 src 里的四边形 [TL,TR,BR,BL] 拉正成 dstW×dstH
void SampleQuad(const Img& src, const float corners[8], int dstW, int dstH,
                std::vector<float>& out) {
    out.assign((size_t)dstW * dstH * 3, 0.0f);
    const float tlx = corners[0], tly = corners[1];
    const float trx = corners[2], try_ = corners[3];
    const float brx = corners[4], bry = corners[5];
    const float blx = corners[6], bly = corners[7];
    for (int y = 0; y < dstH; ++y) {
        double v = (dstH > 1) ? (double)y / (dstH - 1) : 0.0;
        for (int x = 0; x < dstW; ++x) {
            double u = (dstW > 1) ? (double)x / (dstW - 1) : 0.0;
            double topx = tlx + (trx - tlx) * u, topy = tly + (try_ - tly) * u;
            double botx = blx + (brx - blx) * u, boty = bly + (bry - bly) * u;
            double px = topx + (botx - topx) * v;
            double py = topy + (boty - topy) * v;
            if (px < 0) px = 0;
            if (py < 0) py = 0;
            if (px > src.w - 1) px = src.w - 1;
            if (py > src.h - 1) py = src.h - 1;
            int x0 = (int)px, y0 = (int)py;
            int x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
            double wx = px - x0, wy = py - y0;
            float* o = &out[((size_t)y * dstW + x) * 3];
            for (int c = 0; c < 3; ++c) {
                double p00 = src.rgb[((size_t)y0 * src.w + x0) * 3 + c];
                double p01 = src.rgb[((size_t)y0 * src.w + x1) * 3 + c];
                double p10 = src.rgb[((size_t)y1 * src.w + x0) * 3 + c];
                double p11 = src.rgb[((size_t)y1 * src.w + x1) * 3 + c];
                double top = p00 + (p01 - p00) * wx;
                double bot = p10 + (p11 - p10) * wx;
                o[c] = (float)(top + (bot - top) * wy);
            }
        }
    }
}

// ---------- ORT 封装 ----------
struct Rect {
    float cx = 0, cy = 0, w = 0, h = 0, ang = 0;  // 中心、宽高、角度（弧度）
    float score = 0;
    int row = 0;                                  // 所在行（SortIntoRows 填充）
    void corners(float out[8]) const {
        float c = cosf(ang), s = sinf(ang);
        float hw = w * 0.5f, hh = h * 0.5f;
        // TL, TR, BR, BL（屏幕上顺时针）
        const float pts[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
        for (int i = 0; i < 4; ++i) {
            float x = pts[i][0] * c - pts[i][1] * s;
            float y = pts[i][0] * s + pts[i][1] * c;
            out[i * 2] = cx + x;
            out[i * 2 + 1] = cy + y;
        }
    }
};

struct Ctx {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSessionOptions* opts = nullptr;
    OrtSession* det = nullptr;
    OrtSession* rec = nullptr;
    OrtMemoryInfo* memInfo = nullptr;
    std::string detIn, detOut, recIn, recOut;
    std::vector<std::string> dict;  // [0]="" blank, 1..N 字符, 末尾 " "
    std::wstring dir;
    bool ready = false;
    ULONG_PTR gdipToken = 0;   // 本模块自己持有的 GDI+ 引用（--ocr 早于主程序初始化）
    bool gdipOwn = false;
};

Ctx g;
std::mutex g_mu;

std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring U82W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 取会话输入/输出名
std::string SessionIO(const OrtSession* s, bool input) {
    std::string name;
    OrtAllocator* alloc = nullptr;
    if (g.api->GetAllocatorWithDefaultOptions(&alloc) != nullptr) return name;
    char* p = nullptr;
    OrtStatus* st = input ? g.api->SessionGetInputName(s, 0, alloc, &p)
                          : g.api->SessionGetOutputName(s, 0, alloc, &p);
    if (st) { g.api->ReleaseStatus(st); return name; }
    if (p) { name = p; g.api->AllocatorFree(alloc, p); }
    return name;
}

bool LoadDict(const std::wstring& path) {
    FILE* fp = _wfopen(path.c_str(), L"rb");
    if (!fp) return false;
    std::string all;
    char buf[8192];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), fp)) > 0) all.append(buf, rd);
    fclose(fp);
    if (all.empty()) return false;
    // 去掉 BOM
    if (all.size() >= 3 && (unsigned char)all[0] == 0xEF && (unsigned char)all[1] == 0xBB &&
        (unsigned char)all[2] == 0xBF)
        all = all.substr(3);
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= all.size()) {
        size_t nl = all.find('\n', start);
        if (nl == std::string::npos) {
            if (start < all.size()) lines.push_back(all.substr(start));
            break;
        }
        std::string ln = all.substr(start, nl - start);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        lines.push_back(ln);
        start = nl + 1;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.size() < 100) return false;
    g.dict.clear();
    g.dict.push_back("");  // blank
    g.dict.insert(g.dict.end(), lines.begin(), lines.end());
    g.dict.push_back(" ");  // use_space_char
    return true;
}

bool RunSession(OrtSession* s, const std::string& inName, const std::vector<float>& in,
                const int64_t* shape, int dims, const std::string& outName,
                std::vector<float>& out, std::vector<int64_t>& outShape) {
    OrtValue* inT = nullptr;
    OrtStatus* st = g.api->CreateTensorWithDataAsOrtValue(
        g.memInfo, (void*)in.data(), in.size() * sizeof(float), shape, (size_t)dims,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inT);
    if (st) { g.api->ReleaseStatus(st); return false; }
    const char* inNames[1] = {inName.c_str()};
    const char* outNames[1] = {outName.c_str()};
    OrtValue* outT = nullptr;
    st = g.api->Run(s, nullptr, inNames, (const OrtValue* const*)&inT, 1, outNames, 1, &outT);
    g.api->ReleaseValue(inT);
    if (st) { g.api->ReleaseStatus(st); return false; }

    OrtTensorTypeAndShapeInfo* info = nullptr;
    st = g.api->GetTensorTypeAndShape(outT, &info);
    if (st) { g.api->ReleaseStatus(st); g.api->ReleaseValue(outT); return false; }
    size_t n = 0;
    g.api->GetDimensionsCount(info, &n);
    int64_t d[8] = {0};
    if (n > 8) n = 8;
    g.api->GetDimensions(info, d, n);
    g.api->ReleaseTensorTypeAndShapeInfo(info);

    size_t total = 1;
    outShape.assign(d, d + n);
    for (size_t i = 0; i < n; ++i) total *= (size_t)std::max<int64_t>(d[i], 1);

    float* p = nullptr;
    st = g.api->GetTensorMutableData(outT, (void**)&p);
    if (st) { g.api->ReleaseStatus(st); g.api->ReleaseValue(outT); return false; }
    out.assign(p, p + total);
    g.api->ReleaseValue(outT);
    return true;
}

// ---------- GDI+ 解码（保持 PaddleOCR 原生训练的 BGR 通道顺序）----------
bool LoadImage(const std::wstring& path, Img& out) {
    Gdiplus::Bitmap bmp(path.c_str(), FALSE);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;
    int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
    if (w <= 0 || h <= 0) return false;
    Gdiplus::BitmapData bd;
    Gdiplus::Rect r(0, 0, w, h);
    if (bmp.LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bd) != Gdiplus::Ok)
        return false;
    out.w = w; out.h = h;
    out.rgb.resize((size_t)w * h * 3);
    for (int y = 0; y < h; ++y) {
        const unsigned char* src = (const unsigned char*)bd.Scan0 + (size_t)y * bd.Stride;
        unsigned char* dst = &out.rgb[(size_t)y * w * 3];
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 0];  // B
            dst[x * 3 + 1] = src[x * 4 + 1];  // G
            dst[x * 3 + 2] = src[x * 4 + 2];  // R
        }
    }
    bmp.UnlockBits(&bd);
    return out.ok();
}

// 统计图像四边边缘像素的中位数底色
void MedianBorderColor(const Img& src, unsigned char outCol[3]) {
    std::vector<unsigned char> ch[3];
    ch[0].reserve((size_t)(src.w + src.h) * 2);
    ch[1].reserve((size_t)(src.w + src.h) * 2);
    ch[2].reserve((size_t)(src.w + src.h) * 2);
    auto pushPx = [&](int x, int y) {
        const unsigned char* p = &src.rgb[((size_t)y * src.w + x) * 3];
        ch[0].push_back(p[0]);
        ch[1].push_back(p[1]);
        ch[2].push_back(p[2]);
    };
    for (int x = 0; x < src.w; ++x) {
        pushPx(x, 0);
        if (src.h > 1) pushPx(x, src.h - 1);
    }
    for (int y = 1; y < src.h - 1; ++y) {
        pushPx(0, y);
        if (src.w > 1) pushPx(src.w - 1, y);
    }
    for (int c = 0; c < 3; ++c) {
        if (ch[c].empty()) { outCol[c] = 255; continue; }
        size_t mid = ch[c].size() / 2;
        std::nth_element(ch[c].begin(), ch[c].begin() + mid, ch[c].end());
        outCol[c] = ch[c][mid];
    }
}

// 计算图像边缘底色（PP-OCRv5 原生兼容浅色与深色模式，保留原色并用边缘底色扩边即可）
Img PrepareForOcr(const Img& src, unsigned char borderOut[3]) {
    MedianBorderColor(src, borderOut);
    return src;
}

// ---------- det ----------
bool DetPreprocess(const Img& src, const unsigned char borderCol[3], Img& padded,
                   std::vector<float>& tensor, int& nh, int& nw) {
    if (!src.ok()) return false;
    int pad = std::max(kDetPadMin, std::min(64, (int)(0.10 * std::max(src.w, src.h))));
    padded.w = src.w + pad * 2;
    padded.h = src.h + pad * 2;
    padded.rgb.resize((size_t)padded.w * padded.h * 3);
    for (size_t i = 0; i < (size_t)padded.w * padded.h; ++i) {
        padded.rgb[i * 3 + 0] = borderCol[0];
        padded.rgb[i * 3 + 1] = borderCol[1];
        padded.rgb[i * 3 + 2] = borderCol[2];
    }
    for (int y = 0; y < src.h; ++y) {
        memcpy(&padded.rgb[((size_t)(y + pad) * padded.w + pad) * 3],
               &src.rgb[(size_t)y * src.w * 3], (size_t)src.w * 3);
    }
    double scale = 1.0;
    int mn = std::min(padded.w, padded.h), mx = std::max(padded.w, padded.h);
    if (mn < kDetLimitSide) scale = (double)kDetLimitSide / mn;
    if (mx * scale > kDetMaxInput) scale = (double)kDetMaxInput / mx;
    if (scale < 1.0) scale = 1.0;
    nw = (int)std::max(32.0, std::round(padded.w * scale / 32.0) * 32);
    nh = (int)std::max(32.0, std::round(padded.h * scale / 32.0) * 32);
    Img rs = ResizeBilinear(padded, nw, nh);
    if (!rs.ok()) return false;
    tensor.assign((size_t)3 * nh * nw, 0.0f);
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            const unsigned char* p = &rs.rgb[((size_t)y * nw + x) * 3];
            for (int c = 0; c < 3; ++c) {
                float v = p[c] / 255.0f;
                v = (v - kDetMean[c]) / kDetStd[c];
                tensor[(size_t)c * nh * nw + (size_t)y * nw + x] = v;
            }
        }
    }
    return true;
}

// 2x2 膨胀（补笔画断裂）
void Dilate2(std::vector<unsigned char>& m, int w, int h) {
    std::vector<unsigned char> out = m;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!m[(size_t)y * w + x]) continue;
            out[(size_t)y * w + x] = 1;
            if (x + 1 < w) out[(size_t)y * w + x + 1] = 1;
            if (y + 1 < h) out[(size_t)(y + 1) * w + x] = 1;
            if (x + 1 < w && y + 1 < h) out[(size_t)(y + 1) * w + x + 1] = 1;
        }
    }
    m.swap(out);
}

// 连通域 → 矩形（PCA 主轴最小外接框）
void BoxesFromProb(const std::vector<float>& prob, int pw, int ph, double scaleX, double scaleY,
                   int offX, int offY, std::vector<Rect>& boxes) {
    std::vector<unsigned char> mask((size_t)pw * ph, 0);
    for (size_t i = 0; i < prob.size(); ++i) mask[i] = prob[i] > kDetThresh ? 1 : 0;
    Dilate2(mask, pw, ph);

    std::vector<int> label((size_t)pw * ph, 0);
    int next = 1;
    std::vector<int> stack;
    struct Comp {
        double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        int n = 0;
        double scoreSum = 0;
        int minx = 1 << 30, maxx = -1, miny = 1 << 30, maxy = -1;
    };
    std::vector<Comp> comps;

    for (int y = 0; y < ph; ++y) {
        for (int x = 0; x < pw; ++x) {
            size_t idx = (size_t)y * pw + x;
            if (!mask[idx] || label[idx]) continue;
            comps.push_back(Comp());
            Comp c;
            stack.clear();
            stack.push_back((int)idx);
            label[idx] = next;
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cy = cur / pw, cx = cur % pw;
                c.n++;
                c.sx += cx; c.sy += cy;
                c.sxx += (double)cx * cx; c.syy += (double)cy * cy; c.sxy += (double)cx * cy;
                c.scoreSum += prob[(size_t)cur];
                c.minx = std::min(c.minx, cx); c.maxx = std::max(c.maxx, cx);
                c.miny = std::min(c.miny, cy); c.maxy = std::max(c.maxy, cy);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = cx + dx, ny = cy + dy;
                        if (nx < 0 || ny < 0 || nx >= pw || ny >= ph) continue;
                        size_t ni = (size_t)ny * pw + nx;
                        if (mask[ni] && !label[ni]) { label[ni] = next; stack.push_back((int)ni); }
                    }
                }
            }
            comps.back() = c;
            next++;
        }
    }

    for (const Comp& c : comps) {
        if (c.n < 4) continue;
        int bw = c.maxx - c.minx + 1, bh = c.maxy - c.miny + 1;
        if (bw < 3 || bh < 3) continue;
        double score = c.scoreSum / c.n;
        if (score < kDetBoxThresh) continue;
        double mx = c.sx / c.n, my = c.sy / c.n;
        double vxx = c.sxx / c.n - mx * mx;
        double vyy = c.syy / c.n - my * my;
        double vxy = c.sxy / c.n - mx * my;
        double ang = 0.5 * atan2(2 * vxy, vxx - vyy);
        // 屏幕截图文字几乎都是水平排版：短框（如 "1." "2."）或倾角 < 8° 直接用轴对齐框，
        // 避免 PCA 噪点把短数字/符号斜切掉
        double aspect = (double)std::max(bw, bh) / std::max(1, std::min(bw, bh));
        double hw = 0, hh = 0, rcx = 0, rcy = 0;
        if (aspect < 2.2 || fabs(ang) < 0.14) {
            ang = 0.0;
            hw = bw * 0.5;
            hh = bh * 0.5;
            rcx = (c.minx + c.maxx) * 0.5;
            rcy = (c.miny + c.maxy) * 0.5;
        } else {
            double ux = cos(ang), uy = sin(ang);
            double wx = -uy, wy = ux;
            double minT = 1e18, maxT = -1e18, minS = 1e18, maxS = -1e18;
            const double cx4[4] = {(double)c.minx, (double)c.maxx, (double)c.maxx, (double)c.minx};
            const double cy4[4] = {(double)c.miny, (double)c.miny, (double)c.maxy, (double)c.maxy};
            for (int i = 0; i < 4; ++i) {
                double dx = cx4[i] - mx, dy = cy4[i] - my;
                double t = dx * ux + dy * uy;
                double s = dx * wx + dy * wy;
                minT = std::min(minT, t); maxT = std::max(maxT, t);
                minS = std::min(minS, s); maxS = std::max(maxS, s);
            }
            hw = (maxT - minT) * 0.5;
            hh = (maxS - minS) * 0.5;
            rcx = mx + ux * (maxT + minT) * 0.5 + wx * (maxS + minS) * 0.5;
            rcy = my + uy * (maxT + minT) * 0.5 + wy * (maxS + minS) * 0.5;
        }

        // unclip
        double area = hw * hh * 4.0;
        double peri = 2.0 * ((hw * 2) + (hh * 2));
        double dist = peri > 1e-6 ? area * kDetUnclip / peri : 0;
        hw += dist; hh += dist;

        // 映射回原图（去 pad、除缩放）
        Rect r;
        // 注意：det 坐标先乘缩放比，再减 pad 偏移（顺序搞反会整片错位）
        r.cx = (float)(rcx * scaleX - offX);
        r.cy = (float)(rcy * scaleY - offY);
        r.w = (float)(hw * 2 * scaleX);
        r.h = (float)(hh * 2 * scaleY);
        r.ang = (float)ang;
        r.score = (float)score;
        if (r.w < 4 || r.h < 4) continue;
        boxes.push_back(r);
    }
}

// 按行聚类排序
void SortIntoRows(std::vector<Rect>& boxes) {
    if (boxes.size() < 2) return;
    std::vector<float> hs;
    for (const Rect& r : boxes) hs.push_back(r.h);
    std::sort(hs.begin(), hs.end());
    float medH = hs[hs.size() / 2];
    if (medH < 1) medH = 1;
    std::sort(boxes.begin(), boxes.end(), [](const Rect& a, const Rect& b) {
        if (fabsf(a.cy - b.cy) > 1.0f) return a.cy < b.cy;
        return a.cx < b.cx;
    });
    // 分行
    std::vector<std::vector<Rect>> rows;
    for (const Rect& r : boxes) {
        bool placed = false;
        for (auto& row : rows) {
            float rowY = 0;
            for (const Rect& q : row) rowY += q.cy;
            rowY /= (float)row.size();
            if (fabsf(r.cy - rowY) <= medH * 0.6f) { row.push_back(r); placed = true; break; }
        }
        if (!placed) rows.push_back({r});
    }
    std::vector<Rect> out;
    int rowIdx = 0;
    for (auto& row : rows) {
        std::sort(row.begin(), row.end(), [](const Rect& a, const Rect& b) { return a.cx < b.cx; });
        for (Rect r : row) { r.row = rowIdx; out.push_back(r); }
        rowIdx++;
    }
    boxes.swap(out);
}

// ---------- rec ----------
std::string CtcDecode(const std::vector<float>& logits, int T, int C, double& confOut) {
    std::string s;
    int prev = -1;
    double sum = 0;
    int n = 0;
    for (int t = 0; t < T; ++t) {
        const float* row = &logits[(size_t)t * C];
        int best = 0;
        float bv = row[0];
        for (int c = 1; c < C; ++c)
            if (row[c] > bv) { bv = row[c]; best = c; }
        if (best != 0 && best != prev) {
            if ((size_t)best < g.dict.size()) {
                s += g.dict[(size_t)best];
                sum += bv;
                n++;
            }
        }
        prev = best;
    }
    confOut = n ? sum / n : 0.0;
    return s;
}

bool RecOne(const Img& src, const Rect& box, std::string& text, double& conf) {
    float c[8];
    box.corners(c);
    // 宽高比：源框的宽 / 高（PP-OCRv5 官方直接按原框缩放到 48 高，右侧补归一化后的 0.0f）
    float boxW = box.w, boxH = box.h;
    if (boxH < 1.0f) boxH = 1.0f;
    int dstW = (int)lround((double)kRecH * (boxW / boxH));
    dstW = std::max(16, std::min(dstW, kRecMaxW));
    std::vector<float> strip;
    SampleQuad(src, c, dstW, kRecH, strip);

    int canvasW = std::max(dstW, kRecMinW);
    int64_t shape[4] = {1, 3, kRecH, canvasW};
    std::vector<float> in((size_t)3 * kRecH * canvasW, 0.0f);
    for (int y = 0; y < kRecH; ++y) {
        for (int x = 0; x < dstW; ++x) {
            for (int ch = 0; ch < 3; ++ch) {
                float v = (strip[((size_t)y * dstW + x) * 3 + ch] / 255.0f - 0.5f) / 0.5f;
                in[(size_t)ch * kRecH * canvasW + (size_t)y * canvasW + x] = v;
            }
        }
    }
    std::vector<float> out;
    std::vector<int64_t> oshape;
    if (!RunSession(g.rec, g.recIn, in, shape, 4, g.recOut, out, oshape)) return false;
    if (oshape.size() != 3) return false;
    int T = (int)oshape[1];
    int C = (int)oshape[2];
    if (T <= 0 || C <= 0 || (size_t)T * C > out.size()) return false;
    text = CtcDecode(out, T, C, conf);
    return true;
}

// 智能空格整理：只去掉「纯中文字符之间」或行首行尾的多余空白，
// 保留中英文 / 数字 / 行内代码之间的正常空格排版（如 "1 小时"、"将 URL 进行 urlencode 后"）
std::string CleanupSpaces(const std::string& s) {
    std::vector<uint32_t> cps;
    std::vector<std::string> encs;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        int len = 1;
        uint32_t cp = c;
        if (c >= 0xF0) { len = 4; cp = c & 0x07; }
        else if (c >= 0xE0) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xC0) { len = 2; cp = c & 0x1F; }
        if (i + len > s.size()) len = 1;
        for (int k = 1; k < len; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        cps.push_back(cp);
        encs.push_back(s.substr(i, len));
        i += len;
    }
    auto isCjkChar = [](uint32_t cp) {
        return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF);
    };
    auto isCjkPunct = [](uint32_t cp) {
        return (cp >= 0x3001 && cp <= 0x303F) || (cp >= 0xFF01 && cp <= 0xFFEF);
    };
    auto isCjk = [&](uint32_t cp) { return isCjkChar(cp) || isCjkPunct(cp); };
    // 把全角空格 U+3000 一并视为空白处理
    auto isSpace = [](uint32_t cp) { return cp == ' ' || cp == '\t' || cp == 0x3000; };

    std::vector<std::string> keep;
    std::vector<uint32_t> keepCp;
    for (size_t i = 0; i < cps.size(); ++i) {
        uint32_t cp = cps[i];
        if (cp == '\n') {
            while (!keep.empty() && keep.back() == " ") { keep.pop_back(); keepCp.pop_back(); }
            keep.push_back("\n");
            keepCp.push_back('\n');
            continue;
        }
        if (isSpace(cp)) {
            // 找前一个非空白字符和后一个非空白字符
            uint32_t prev = keepCp.empty() ? 0 : keepCp.back();
            if (prev == 0 || prev == '\n' || prev == ' ') continue;  // 跳过行首空白与连续空白
            size_t j = i + 1;
            while (j < cps.size() && isSpace(cps[j])) ++j;
            uint32_t next = (j < cps.size()) ? cps[j] : 0;
            if (next == 0 || next == '\n') continue;  // 跳过行尾空白

            // 仅当两侧都是中文/中文标点时去掉多余空格；保留中文与英文/数字/代码间的正常空格
            if (isCjk(prev) && isCjk(next)) continue;
            // 左括号后、右括号前紧贴英文时也去掉多余内空格（如 "（ 比如" / "接口 ）"）
            if (prev == 0xFF08 || prev == '(' || next == 0xFF09 || next == ')') continue;

            keep.push_back(" ");
            keepCp.push_back(' ');
        } else {
            keep.push_back(encs[i]);
            keepCp.push_back(cp);
        }
    }
    while (!keep.empty() && keep.back() == " ") keep.pop_back();
    std::string out;
    for (const std::string& k : keep) out += k;
    return out;
}

// ---------- 行分片合并 + 自然段智能聚合（供图内原位翻译使用）----------

struct RawBoxText {
    RECT rc{};
    float h = 0.0f;
    int row = 0;
    double conf = 0.0;
    std::string textU8;
};

struct LineItem {
    RECT rc{};
    float avgH = 0.0f;
    int row = 0;
    int count = 0;
    std::string textU8;
    std::wstring text;
};

static RECT BoxToClampedRect(const Rect& b, int imgW, int imgH) {
    float c[8];
    b.corners(c);
    float minX = c[0], maxX = c[0], minY = c[1], maxY = c[1];
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, c[i * 2]);
        maxX = std::max(maxX, c[i * 2]);
        minY = std::min(minY, c[i * 2 + 1]);
        maxY = std::max(maxY, c[i * 2 + 1]);
    }
    RECT r;
    r.left = std::max(0, std::min(imgW - 1, (int)std::floor(minX)));
    r.top = std::max(0, std::min(imgH - 1, (int)std::floor(minY)));
    r.right = std::max((int)r.left + 1, std::min(imgW, (int)std::ceil(maxX)));
    r.bottom = std::max((int)r.top + 1, std::min(imgH, (int)std::ceil(maxY)));
    return r;
}

static bool StartsWithListMarker(const std::wstring& s) {
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    if (i >= s.size()) return false;
    wchar_t c = s[i];
    if (c == L'•' || c == L'·' || c == L'▪' || c == L'▫' || c == L'◆' || c == L'◇' ||
        c == L'★' || c == L'☆' || (c >= 0x2460 && c <= 0x2473)) {
        return true;
    }
    if ((c == L'-' || c == L'*' || c == L'+') && i + 1 < s.size() && s[i + 1] == L' ') {
        return true;
    }
    // 1. / 2) / (1)
    if (c == L'(' || c == L'（') {
        size_t j = i + 1;
        while (j < s.size() && iswdigit(s[j])) ++j;
        if (j > i + 1 && j < s.size() && (s[j] == L')' || s[j] == L'）')) return true;
    }
    if (iswdigit(c)) {
        size_t j = i;
        while (j < s.size() && iswdigit(s[j])) ++j;
        if (j > i && j - i <= 3 && j < s.size() && (s[j] == L'.' || s[j] == L')' || s[j] == L'、')) {
            if (j + 1 >= s.size() || s[j] == L'、' || s[j + 1] == L' ') return true;
        }
    }
    return false;
}

static bool IsCjkWide(wchar_t ch) {
    return (ch >= 0x2E80 && ch <= 0x9FFF) || (ch >= 0xF900 && ch <= 0xFAFF) ||
           (ch >= 0x3001 && ch <= 0x303F) || (ch >= 0xFF01 && ch <= 0xFFEF);
}

static void AppendParagraphLineText(std::wstring& acc, const std::wstring& nextLine) {
    if (nextLine.empty()) return;
    if (acc.empty()) {
        acc = nextLine;
        return;
    }
    wchar_t last = acc.back();
    wchar_t first = nextLine.front();
    // 英文行尾连字符断词（如 notifi- + cations）：若连字符前是字母且下一行是小写字母，智能拼回完整单词
    if (last == L'-' && acc.size() >= 2 && iswalpha(acc[acc.size() - 2]) &&
        (first >= L'a' && first <= L'z')) {
        // 常见复合词（如 non-stop / twenty-five）保留连字符，长词断行也兼容大模型理解，直接紧接不加空格
        acc += nextLine;
        return;
    }
    if (IsCjkWide(last) || IsCjkWide(first)) {
        acc += nextLine;
    } else {
        if (last != L' ') acc += L' ';
        acc += nextLine;
    }
}

static void ClusterIntoBlocks(const std::vector<RawBoxText>& raws, std::vector<OcrBlock>& outBlocks) {
    outBlocks.clear();
    if (raws.empty()) return;

    // 1) 同行且同列的分片合并成 LineItem（跨列大间距保持独立，避免左右分栏被拼成一行）
    std::vector<LineItem> lines;
    for (const auto& rb : raws) {
        std::string cleaned = CleanupSpaces(rb.textU8);
        if (cleaned.empty()) continue;
        if (!lines.empty() && lines.back().row == rb.row) {
            LineItem& prev = lines.back();
            float refH = std::max(prev.avgH, rb.h);
            int gap = rb.rc.left - prev.rc.right;
            bool sameCol = (gap <= (int)(refH * 2.2f)) &&
                           (std::max(prev.avgH, rb.h) <= std::min(prev.avgH, rb.h) * 1.65f);
            if (sameCol) {
                if (gap > (int)(rb.h * 0.28f) && !prev.textU8.empty() && prev.textU8.back() != ' ') {
                    prev.textU8 += " ";
                }
                prev.textU8 += cleaned;
                prev.rc.left = std::min(prev.rc.left, rb.rc.left);
                prev.rc.top = std::min(prev.rc.top, rb.rc.top);
                prev.rc.right = std::max(prev.rc.right, rb.rc.right);
                prev.rc.bottom = std::max(prev.rc.bottom, rb.rc.bottom);
                prev.avgH = (prev.avgH * prev.count + rb.h) / (prev.count + 1);
                prev.count++;
                continue;
            }
        }
        LineItem li;
        li.rc = rb.rc;
        li.avgH = rb.h;
        li.row = rb.row;
        li.count = 1;
        li.textU8 = cleaned;
        lines.push_back(li);
    }

    for (auto& li : lines) {
        li.textU8 = CleanupSpaces(li.textU8);
        li.text = U82W(li.textU8);
    }

    // 按垂直中心从上到下排序，同行从左到右
    std::sort(lines.begin(), lines.end(), [](const LineItem& a, const LineItem& b) {
        int cya = (a.rc.top + a.rc.bottom) / 2;
        int cyb = (b.rc.top + b.rc.bottom) / 2;
        if (std::abs(cya - cyb) > 4) return cya < cyb;
        return a.rc.left < b.rc.left;
    });

    // 2) 把属于同一自然段的多行合并成 OcrBlock（支持左右双栏独立成段，不合并垂直菜单/列表）
    struct BuildBlock {
        OcrBlock blk;
        int bodyLeft = 0;  // 正文左边界（兼容首行缩进）
        std::wstring lastLineText;
    };
    std::vector<BuildBlock> built;

    for (const auto& li : lines) {
        if (li.text.empty()) continue;
        float cyL = 0.5f * (li.rc.top + li.rc.bottom);
        int wL = li.rc.right - li.rc.left;

        int bestIdx = -1;
        float bestDist = 1e9f;

        if (!StartsWithListMarker(li.text)) {
            for (int bi = (int)built.size() - 1; bi >= 0; --bi) {
                const BuildBlock& cand = built[bi];
                const RECT& lastRc = cand.blk.lineRcs.back();
                float cyPrev = 0.5f * (lastRc.top + lastRc.bottom);
                float avgH = 0.5f * (cand.blk.avgLineH + li.avgH);
                if (avgH < 1.0f) avgH = 1.0f;

                // 必须在上一行正下方，且行间距紧凑（中间有空行则自然分段）
                float centerDy = cyL - cyPrev;
                int vGap = li.rc.top - lastRc.bottom;
                if (centerDy < avgH * 0.45f) continue;
                if (centerDy > avgH * 1.92f || vGap > (int)(avgH * 0.80f)) continue;

                // 字号必须相近（标题与正文不合并）
                if (std::max(cand.blk.avgLineH, li.avgH) > std::min(cand.blk.avgLineH, li.avgH) * 1.36f)
                    continue;

                // 左边界对齐，或首行缩进（如首行右缩 2 字符）
                bool leftAligned = std::abs(li.rc.left - cand.bodyLeft) <= (int)(avgH * 1.65f);
                bool firstLineIndent = (cand.blk.lineCount == 1) && (cand.blk.rc.left > li.rc.left) &&
                                       (cand.blk.rc.left - li.rc.left <= (int)(avgH * 2.9f));
                if (!leftAligned && !firstLineIndent) continue;

                // 水平投影有足够重叠（防跨列串行）
                int ovL = std::max(cand.blk.rc.left, li.rc.left);
                int ovR = std::min(cand.blk.rc.right, li.rc.right);
                int minW = std::min<int>(cand.blk.rc.right - cand.blk.rc.left, wL);
                if (ovR - ovL < (int)(minW * 0.35f)) continue;

                // 关键防误合：只有当上一行确实是「段落折行」（宽度够长、且延伸到了右边界附近）才继续合并；
                // 垂直导航菜单、独立短标签、以冒号结尾的小标题均保持独立块！
                int maxRight = std::max(cand.blk.rc.right, li.rc.right);
                int candW = cand.blk.rc.right - cand.blk.rc.left;
                if (candW < (int)(avgH * 5.6f)) continue;
                if (lastRc.right < maxRight - (int)(avgH * 4.5f)) continue;
                if (!cand.lastLineText.empty()) {
                    wchar_t tail = cand.lastLineText.back();
                    if (tail == L':' || tail == L'：') continue;
                }

                if (centerDy < bestDist) {
                    bestDist = centerDy;
                    bestIdx = bi;
                }
            }
        }

        if (bestIdx >= 0) {
            BuildBlock& dst = built[bestIdx];
            if (dst.blk.lineCount == 1 && li.rc.left < dst.bodyLeft) {
                dst.bodyLeft = li.rc.left;
            }
            dst.blk.rc.left = std::min(dst.blk.rc.left, li.rc.left);
            dst.blk.rc.top = std::min(dst.blk.rc.top, li.rc.top);
            dst.blk.rc.right = std::max(dst.blk.rc.right, li.rc.right);
            dst.blk.rc.bottom = std::max(dst.blk.rc.bottom, li.rc.bottom);
            dst.blk.avgLineH =
                (dst.blk.avgLineH * dst.blk.lineCount + li.avgH) / (dst.blk.lineCount + 1);
            dst.blk.lineCount++;
            dst.blk.lineRcs.push_back(li.rc);
            AppendParagraphLineText(dst.blk.text, li.text);
            dst.lastLineText = li.text;
        } else {
            BuildBlock nb;
            nb.blk.rc = li.rc;
            nb.blk.lineRcs.push_back(li.rc);
            nb.blk.avgLineH = li.avgH;
            nb.blk.lineCount = 1;
            nb.blk.text = li.text;
            nb.bodyLeft = li.rc.left;
            nb.lastLineText = li.text;
            built.push_back(std::move(nb));
        }
    }

    outBlocks.reserve(built.size());
    for (auto& b : built) {
        outBlocks.push_back(std::move(b.blk));
    }
}

static bool RunDetAndRec(const Img& rawImg, std::vector<RawBoxText>& outRaws, int* outMs,
                         double* outConf) {
    outRaws.clear();
    if (!g.ready || !rawImg.ok()) return false;
    ULONGLONG t0 = GetTickCount64();

    unsigned char borderCol[3] = {255, 255, 255};
    Img img = PrepareForOcr(rawImg, borderCol);

    Img padded;
    std::vector<float> detIn;
    int nh = 0, nw = 0;
    if (!DetPreprocess(img, borderCol, padded, detIn, nh, nw)) return false;
    int64_t dshape[4] = {1, 3, nh, nw};
    std::vector<float> prob;
    std::vector<int64_t> pshape;
    if (!RunSession(g.det, g.detIn, detIn, dshape, 4, g.detOut, prob, pshape)) return false;
    if (pshape.size() != 4 || pshape[2] != nh || pshape[3] != nw) return false;

    int offX = (padded.w - img.w) / 2;
    int offY = (padded.h - img.h) / 2;
    double scaleX = (double)padded.w / nw;
    double scaleY = (double)padded.h / nh;
    std::vector<Rect> boxes;
    BoxesFromProb(prob, nw, nh, scaleX, scaleY, offX, offY, boxes);
    if (boxes.empty()) {
        if (outMs) *outMs = (int)(GetTickCount64() - t0);
        if (outConf) *outConf = 0.0;
        return true;
    }
    SortIntoRows(boxes);

    double confSum = 0.0;
    int confN = 0;
    outRaws.reserve(boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i) {
        const Rect& b = boxes[i];
        std::string txt;
        double conf = 0.0;
        if (!RecOne(img, b, txt, conf)) continue;
        if (txt.empty()) continue;
        confSum += conf;
        confN++;

        RawBoxText rb;
        rb.rc = BoxToClampedRect(b, img.w, img.h);
        rb.h = b.h;
        rb.row = b.row;
        rb.conf = conf;
        rb.textU8 = std::move(txt);
        outRaws.push_back(std::move(rb));
    }

    if (outMs) *outMs = (int)(GetTickCount64() - t0);
    if (outConf) *outConf = confN ? confSum / confN : 0.0;
    return true;
}

// 主流程（纯文本拼接）
std::wstring Recognize(const Img& rawImg, int* outMs, double* outConf) {
    std::vector<RawBoxText> raws;
    if (!RunDetAndRec(rawImg, raws, outMs, outConf) || raws.empty()) return std::wstring();

    std::string result;
    int prevRow = -1;
    int prevRight = -1;
    for (const auto& b : raws) {
        if (b.row != prevRow) {
            if (!result.empty()) result += "\n";
            prevRow = b.row;
            prevRight = -1;
        } else if (prevRight >= 0) {
            float gap = (float)(b.rc.left - prevRight);
            if (gap > b.h * 3.0f) {
                // 跨列（左侧菜单 + 右侧内容）：换行而不是拼在一行
                if (!result.empty() && result.back() != '\n') result += "\n";
            } else if (gap > b.h * 0.35f && !result.empty() && result.back() != ' ' &&
                       result.back() != '\n') {
                result += " ";
            }
        }
        result += b.textU8;
        prevRight = b.rc.right;
    }
    result = CleanupSpaces(result);
    return U82W(result);
}

}  // namespace

static bool InitUnlocked(const std::wstring& modelDir) {
    if (g.ready) return true;
    g.dir = modelDir;
    if (!g.dir.empty() && g.dir.back() != L'\\' && g.dir.back() != L'/') g.dir += L'\\';

    // 1) onnxruntime.dll：先找模型目录，再找 exe 目录
    std::wstring dllPath = g.dir + L"onnxruntime.dll";
    if (!FileExists(dllPath)) return false;
    g.dll = LoadLibraryW(dllPath.c_str());
    if (!g.dll) return false;
    typedef const OrtApiBase*(ORT_API_CALL * GetApiBaseFn)(void);
    GetApiBaseFn getBase = (GetApiBaseFn)GetProcAddress(g.dll, "OrtGetApiBase");
    if (!getBase) { FreeLibrary(g.dll); g.dll = nullptr; return false; }
    const OrtApiBase* base = getBase();
    if (!base) { FreeLibrary(g.dll); g.dll = nullptr; return false; }
    g.api = base->GetApi(ORT_API_VERSION);
    if (!g.api) { FreeLibrary(g.dll); g.dll = nullptr; return false; }

    // 2) 字典
    auto cleanupUnlocked = [&]() {
        if (g.api) {
            if (g.det) { g.api->ReleaseSession(g.det); g.det = nullptr; }
            if (g.rec) { g.api->ReleaseSession(g.rec); g.rec = nullptr; }
            if (g.memInfo) { g.api->ReleaseMemoryInfo(g.memInfo); g.memInfo = nullptr; }
            if (g.opts) { g.api->ReleaseSessionOptions(g.opts); g.opts = nullptr; }
            if (g.env) { g.api->ReleaseEnv(g.env); g.env = nullptr; }
        }
        if (g.dll) { FreeLibrary(g.dll); g.dll = nullptr; }
        if (g.gdipOwn) { Gdiplus::GdiplusShutdown(g.gdipToken); g.gdipOwn = false; }
        g.api = nullptr;
        g.ready = false;
    };

    if (!LoadDict(g.dir + L"ppocr_keys.txt")) { cleanupUnlocked(); return false; }

    // 2.5) GDI+（Bitmap 解码需要；主程序 UI 初始化在 --ocr 之后，这里自己持一份）
    if (!g.gdipOwn) {
        Gdiplus::GdiplusStartupInput si;
        if (Gdiplus::GdiplusStartup(&g.gdipToken, &si, nullptr) == Gdiplus::Ok) g.gdipOwn = true;
    }

    // 3) env + 两个会话
    if (g.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "zpix", &g.env)) { cleanupUnlocked(); return false; }
    if (g.api->CreateSessionOptions(&g.opts)) { cleanupUnlocked(); return false; }
    g.api->SetIntraOpNumThreads(g.opts, 2);
    g.api->SetSessionGraphOptimizationLevel(g.opts, ORT_ENABLE_ALL);
    if (g.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &g.memInfo)) {
        cleanupUnlocked();
        return false;
    }
    std::wstring detPath = g.dir + L"det.onnx";
    std::wstring recPath = g.dir + L"rec.onnx";
    if (!FileExists(detPath) || !FileExists(recPath)) { cleanupUnlocked(); return false; }
    if (g.api->CreateSession(g.env, detPath.c_str(), g.opts, &g.det)) { cleanupUnlocked(); return false; }
    if (g.api->CreateSession(g.env, recPath.c_str(), g.opts, &g.rec)) { cleanupUnlocked(); return false; }
    g.detIn = SessionIO(g.det, true);
    g.detOut = SessionIO(g.det, false);
    g.recIn = SessionIO(g.rec, true);
    g.recOut = SessionIO(g.rec, false);
    if (g.detIn.empty() || g.detOut.empty() || g.recIn.empty() || g.recOut.empty()) {
        cleanupUnlocked();
        return false;
    }
    g.ready = true;
    return true;
}

bool Init(const std::wstring& modelDir) {
    std::lock_guard<std::mutex> lk(g_mu);
    return InitUnlocked(modelDir);
}

bool EnsureReady() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g.ready) return true;

    wchar_t modPath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, modPath, MAX_PATH);
    wchar_t* slash = wcsrchr(modPath, L'\\');
    if (slash) *(slash + 1) = 0;
    std::wstring exeDir = modPath;

    if (InitUnlocked(exeDir + L"model\\")) return true;

    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (localAppData && localAppData[0]) {
        std::wstring alt = std::wstring(localAppData) + L"\\Zpix\\model\\";
        if (InitUnlocked(alt)) return true;
    }
    const wchar_t* appData = _wgetenv(L"APPDATA");
    if (appData && appData[0]) {
        std::wstring alt = std::wstring(appData) + L"\\Zpix\\model\\";
        if (InitUnlocked(alt)) return true;
    }
    return false;
}

bool Ready() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g.ready;
}

std::wstring RecognizeFile(const std::wstring& imagePath, int* outMs, double* outConf) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g.ready) return std::wstring();
    Img img;
    if (!LoadImage(imagePath, img)) return std::wstring();
    return Recognize(img, outMs, outConf);
}

static bool BgraToImg(const unsigned char* bgra, int width, int height, int stride, Img& img) {
    if (!bgra || width <= 0 || height <= 0) return false;
    img.w = width;
    img.h = height;
    img.rgb.resize((size_t)width * height * 3);
    for (int y = 0; y < height; ++y) {
        const unsigned char* s = bgra + (size_t)y * stride;
        unsigned char* d = &img.rgb[(size_t)y * width * 3];
        for (int x = 0; x < width; ++x) {
            d[x * 3 + 0] = s[x * 4 + 0];  // B
            d[x * 3 + 1] = s[x * 4 + 1];  // G
            d[x * 3 + 2] = s[x * 4 + 2];  // R
        }
    }
    return true;
}

std::wstring RecognizeBgra(const unsigned char* bgra, int width, int height, int stride,
                           int* outMs, double* outConf) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g.ready) return std::wstring();
    Img img;
    if (!BgraToImg(bgra, width, height, stride, img)) return std::wstring();
    return Recognize(img, outMs, outConf);
}

bool RecognizeBlocksBgra(const unsigned char* bgra, int width, int height, int stride,
                         std::vector<OcrBlock>& outBlocks, int* outMs) {
    outBlocks.clear();
    if (!EnsureReady()) return false;
    Img img;
    if (!BgraToImg(bgra, width, height, stride, img)) return false;

    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<RawBoxText> raws;
    double conf = 0.0;
    if (!RunDetAndRec(img, raws, outMs, &conf)) return false;
    ClusterIntoBlocks(raws, outBlocks);
    return true;
}

bool RecognizeBlocksFile(const std::wstring& imagePath, std::vector<OcrBlock>& outBlocks,
                         int* outMs) {
    outBlocks.clear();
    if (!EnsureReady()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    Img img;
    if (!LoadImage(imagePath, img)) return false;
    std::vector<RawBoxText> raws;
    double conf = 0.0;
    if (!RunDetAndRec(img, raws, outMs, &conf)) return false;
    ClusterIntoBlocks(raws, outBlocks);
    return true;
}

void Shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g.api) {
        if (g.det) { g.api->ReleaseSession(g.det); g.det = nullptr; }
        if (g.rec) { g.api->ReleaseSession(g.rec); g.rec = nullptr; }
        if (g.memInfo) { g.api->ReleaseMemoryInfo(g.memInfo); g.memInfo = nullptr; }
        if (g.opts) { g.api->ReleaseSessionOptions(g.opts); g.opts = nullptr; }
        if (g.env) { g.api->ReleaseEnv(g.env); g.env = nullptr; }
    }
    if (g.dll) { FreeLibrary(g.dll); g.dll = nullptr; }
    if (g.gdipOwn) { Gdiplus::GdiplusShutdown(g.gdipToken); g.gdipOwn = false; }
    g.api = nullptr;
    g.ready = false;
}

}  // namespace ocr_paddle