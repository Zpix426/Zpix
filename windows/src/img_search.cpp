#include "img_search.h"
#include "translate.h"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace img_search {
namespace {

static std::string WideToU8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len, NULL, NULL);
    return out;
}

static std::wstring U8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

static std::string UrlEncodeAscii(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

static std::wstring UrlEncodeWide(const std::wstring& s) {
    return U8ToWide(UrlEncodeAscii(WideToU8(s)));
}

static std::wstring TrimW(std::wstring s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n'))
        s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n'))
        s.pop_back();
    return s;
}

// 解析 JSON 字符串字面量（正确处理 \/ 转义为 /，避免 https:\/\/i.ibb.co 斜杠残留反斜杠）
static std::wstring ParseJsonStringAt(const std::string& s, size_t& p) {
    std::string buf;
    auto putUtf8 = [&buf](unsigned cp) {
        if (cp < 0x80) {
            buf.push_back((char)cp);
        } else if (cp < 0x800) {
            buf.push_back((char)(0xC0 | (cp >> 6)));
            buf.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            buf.push_back((char)(0xE0 | (cp >> 12)));
            buf.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            buf.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            buf.push_back((char)(0xF0 | (cp >> 18)));
            buf.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            buf.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            buf.push_back((char)(0x80 | (cp & 0x3F)));
        }
    };
    while (p < s.size()) {
        unsigned char c = (unsigned char)s[p++];
        if (c == '"') break;
        if (c != '\\') {
            buf.push_back((char)c);
            continue;
        }
        if (p >= s.size()) break;
        char e = s[p++];
        switch (e) {
        case 'n': buf.push_back('\n'); break;
        case 'r': buf.push_back('\r'); break;
        case 't': buf.push_back('\t'); break;
        case '/': buf.push_back('/'); break;
        case '\\': buf.push_back('\\'); break;
        case '"': buf.push_back('"'); break;
        case 'u': {
            if (p + 4 > s.size()) return U8ToWide(buf);
            auto hexv = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return 0;
            };
            unsigned cu = (hexv(s[p]) << 12) | (hexv(s[p + 1]) << 8) | (hexv(s[p + 2]) << 4) | hexv(s[p + 3]);
            p += 4;
            putUtf8(cu);
            break;
        }
        default: buf.push_back(e); break;
        }
    }
    return U8ToWide(buf);
}

// 从 startFrom 起精确查找 "key": "value"（双引号精确闭合，绝不会把 "url_viewer" 误当成 "url"）
static std::wstring JsonFindExactString(const std::string& s, const char* key, size_t startFrom = 0) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = startFrom;
    while (pos < s.size()) {
        size_t p = s.find(pat, pos);
        if (p == std::string::npos) return L"";
        size_t q = p + pat.size();
        while (q < s.size() && (s[q] == ' ' || s[q] == '\t' || s[q] == '\r' || s[q] == '\n')) ++q;
        if (q >= s.size() || s[q] != ':') {
            pos = p + pat.size();
            continue;
        }
        ++q;
        while (q < s.size() && (s[q] == ' ' || s[q] == '\t' || s[q] == '\r' || s[q] == '\n')) ++q;
        if (q >= s.size() || s[q] != '"') {
            pos = q;
            continue;
        }
        ++q;
        return ParseJsonStringAt(s, q);
    }
    return L"";
}

// ---------------------------------------------------------------- GDI+ 内存编码（JPG / PNG）

static int FindEncoderClsid(const WCHAR* mime, CLSID* out) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
    Gdiplus::GetImageEncoders(num, size, codecs);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) {
            *out = codecs[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

static bool EncodeImagePayload(const Bitmap32& bm, std::vector<uint8_t>& outBytes,
                               std::string& outFileName, std::string& outMime) {
    outBytes.clear();
    if (bm.empty()) return false;
    InitGdiPlus();

    bool useJpeg = ((long long)bm.w * bm.h > 160000LL);
    CLSID clsid{};
    if (useJpeg && FindEncoderClsid(L"image/jpeg", &clsid) >= 0) {
        outFileName = "zpix.jpg";
        outMime = "image/jpeg";
    } else if (FindEncoderClsid(L"image/png", &clsid) >= 0) {
        useJpeg = false;
        outFileName = "zpix.png";
        outMime = "image/png";
    } else {
        return false;
    }

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK) return false;

    bool ok = false;
    BYTE* bits = const_cast<BYTE*>((const BYTE*)bm.px.data());
    Gdiplus::Bitmap bmp(bm.w, bm.h, bm.w * 4, PixelFormat32bppARGB, bits);
    Gdiplus::EncoderParameters eps{};
    ULONG quality = 90;
    if (useJpeg) {
        eps.Count = 1;
        eps.Parameter[0].Guid = Gdiplus::EncoderQuality;
        eps.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        eps.Parameter[0].NumberOfValues = 1;
        eps.Parameter[0].Value = &quality;
    }

    if (bmp.Save(stream, &clsid, useJpeg ? &eps : NULL) == Gdiplus::Ok) {
        HGLOBAL h = NULL;
        if (GetHGlobalFromStream(stream, &h) == S_OK && h) {
            SIZE_T n = GlobalSize(h);
            const uint8_t* p = (const uint8_t*)GlobalLock(h);
            if (p && n > 0) {
                outBytes.assign(p, p + n);
                GlobalUnlock(h);
                ok = true;
            }
        }
    }
    stream->Release();
    return ok;
}

// ---------------------------------------------------------------- ImgBB 直链上传（10 分钟自动销毁）

static bool UploadToImgBB(const std::vector<uint8_t>& fileBytes, const std::string& fileName,
                          const std::string& mime, const std::wstring& apiKey,
                          std::wstring& outDirectUrl, std::wstring& outErr) {
    outDirectUrl.clear();
    std::wstring key = TrimW(apiKey);
    if (key.empty()) {
        outErr = L"未配置 ImgBB API Key";
        return false;
    }

    const std::string boundary = "----ZpixImgBBBoundary7MA4YWxkTrZu0gW";
    std::string body;
    body.reserve(fileBytes.size() + 320);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; filename=\"" + fileName + "\"\r\n";
    body += "Content-Type: " + mime + "\r\n\r\n";
    body.append((const char*)fileBytes.data(), fileBytes.size());
    body += "\r\n--" + boundary + "--\r\n";

    std::wstring url = L"https://api.imgbb.com/1/upload?expiration=600&key=" + UrlEncodeWide(key);
    std::wstring headers =
        L"Content-Type: multipart/form-data; boundary=" + U8ToWide(boundary) + L"\r\n";

    std::string resp;
    DWORD httpErr = 0;
    if (!HttpRequest(L"POST", url, headers, body, resp, &httpErr)) {
        std::wstring apiMsg = JsonFindExactString(resp, "message", 0);
        if (!apiMsg.empty()) {
            outErr = L"ImgBB: " + apiMsg;
        } else {
            outErr = L"ImgBB 上传失败 (HTTP " + std::to_wstring(httpErr) + L")";
        }
        return false;
    }

    // 精确从 "data" 节点内读取图片直链 "url"（即 https://i.ibb.co/...，绝非查看页 url_viewer）
    size_t dataPos = resp.find("\"data\"");
    size_t searchStart = (dataPos == std::string::npos) ? 0 : dataPos;
    std::wstring direct = JsonFindExactString(resp, "url", searchStart);
    if (direct.empty()) {
        direct = JsonFindExactString(resp, "display_url", searchStart);
    }
    if (direct.compare(0, 4, L"http") != 0) {
        outErr = L"ImgBB 未返回有效直链";
        return false;
    }
    outDirectUrl = direct;
    ZLog(L"[img-search] imgbb direct=%ls", outDirectUrl.c_str());
    return true;
}

// 备用匿名 1h 临时直链（当未填 Key 或 ImgBB 偶发网络波动时兜底）
static bool UploadToLitterboxFallback(const std::vector<uint8_t>& fileBytes,
                                      const std::string& fileName, const std::string& mime,
                                      std::wstring& outDirectUrl, std::wstring& outErr) {
    outDirectUrl.clear();
    const std::string boundary = "----ZpixLitterBoundary9kP2LmQ";
    std::string body;
    body.reserve(fileBytes.size() + 512);
    auto addField = [&](const char* name, const char* val) {
        body += "--" + boundary + "\r\n";
        body += std::string("Content-Disposition: form-data; name=\"") + name + "\"\r\n\r\n";
        body += std::string(val) + "\r\n";
    };
    addField("reqtype", "fileupload");
    addField("time", "1h");
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"" + fileName + "\"\r\n";
    body += "Content-Type: " + mime + "\r\n\r\n";
    body.append((const char*)fileBytes.data(), fileBytes.size());
    body += "\r\n--" + boundary + "--\r\n";

    std::wstring headers =
        L"Content-Type: multipart/form-data; boundary=" + U8ToWide(boundary) + L"\r\n";
    std::string resp;
    DWORD httpErr = 0;
    if (!HttpRequest(L"POST", L"https://litterbox.catbox.moe/resources/internals/api.php", headers,
                     body, resp, &httpErr)) {
        outErr = L"备用图床上传失败 (HTTP " + std::to_wstring(httpErr) + L")";
        return false;
    }
    std::wstring u = TrimW(U8ToWide(resp));
    if (u.compare(0, 8, L"https://") != 0) {
        outErr = L"备用图床返回异常";
        return false;
    }
    outDirectUrl = u;
    ZLog(L"[img-search] litterbox fallback direct=%ls", outDirectUrl.c_str());
    return true;
}

// 百度识图原生二进制直传（免第三方图床，国内直连秒开）
static bool UploadToBaiduDirect(const std::vector<uint8_t>& fileBytes, const std::string& fileName,
                                const std::string& mime, std::wstring& outLandingUrl,
                                std::wstring& outErr) {
    outLandingUrl.clear();
    const std::string boundary = "----ZpixBaiduBoundary8X3Kp9W";
    std::string body;
    body.reserve(fileBytes.size() + 512);
    auto addField = [&](const char* name, const char* val) {
        body += "--" + boundary + "\r\n";
        body += std::string("Content-Disposition: form-data; name=\"") + name + "\"\r\n\r\n";
        body += std::string(val) + "\r\n";
    };
    addField("tn", "pc");
    addField("from", "pc");
    addField("image_source", "PC_UPLOAD_SEARCH_FILE");
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"image\"; filename=\"" + fileName + "\"\r\n";
    body += "Content-Type: " + mime + "\r\n\r\n";
    body.append((const char*)fileBytes.data(), fileBytes.size());
    body += "\r\n--" + boundary + "--\r\n";

    std::wstring headers =
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n"
        L"Referer: https://graph.baidu.com/\r\n"
        L"Content-Type: multipart/form-data; boundary=" +
        U8ToWide(boundary) + L"\r\n";

    std::string resp;
    DWORD httpErr = 0;
    if (!HttpRequest(L"POST", L"https://graph.baidu.com/upload", headers, body, resp, &httpErr)) {
        outErr = L"百度识图直传失败 (HTTP " + std::to_wstring(httpErr) + L")";
        return false;
    }
    size_t dataPos = resp.find("\"data\"");
    size_t searchStart = (dataPos == std::string::npos) ? 0 : dataPos;
    std::wstring landing = JsonFindExactString(resp, "url", searchStart);
    if (landing.compare(0, 4, L"http") != 0) {
        outErr = L"百度识图未返回跳转链接";
        return false;
    }
    outLandingUrl = landing;
    ZLog(L"[img-search] baidu landing=%ls", outLandingUrl.c_str());
    return true;
}

}  // namespace

const wchar_t* EngineLabel(int engine) {
    switch (engine) {
    case IMG_ENGINE_YANDEX: return L"Yandex";
    case IMG_ENGINE_GOOGLE: return L"Google";
    case IMG_ENGINE_BING: return L"Bing";
    case IMG_ENGINE_BAIDU: return L"百度";
    case IMG_ENGINE_SAUCENAO: return L"SauceNAO";
    case IMG_ENGINE_TRACEMOE: return L"Trace.moe";
    default: return L"Yandex";
    }
}

const wchar_t* EngineDesc(int engine) {
    switch (engine) {
    case IMG_ENGINE_YANDEX: return L"Yandex（找高清原图/全网出处最强）";
    case IMG_ENGINE_GOOGLE: return L"Google Lens（综合识物/外网搜索）";
    case IMG_ENGINE_BING: return L"Bing 识图（必应国际版视觉搜索）";
    case IMG_ENGINE_BAIDU: return L"百度识图（国内商品/表情包原生直传）";
    case IMG_ENGINE_SAUCENAO: return L"SauceNAO（插画/Pixiv 原作溯源）";
    case IMG_ENGINE_TRACEMOE: return L"Trace.moe（动漫截图定位番剧集数）";
    default: return L"Yandex";
    }
}

bool UploadAndBuildSearchUrl(const Bitmap32& img, int engine, const std::wstring& imgbbKey,
                             SearchResult& out) {
    out = SearchResult{};
    if (engine < 0 || engine >= IMG_ENGINE_COUNT) engine = IMG_ENGINE_YANDEX;
    out.engine = engine;
    out.engineName = EngineLabel(engine);

    std::vector<uint8_t> bytes;
    std::string fileName, mime;
    if (!EncodeImagePayload(img, bytes, fileName, mime)) {
        out.error = L"图片编码失败";
        return false;
    }

    // 百度识图优先走官方免图床直传（速度最快），若失败再回退 ImgBB 直链
    if (engine == IMG_ENGINE_BAIDU) {
        std::wstring landing, bdErr;
        if (UploadToBaiduDirect(bytes, fileName, mime, landing, bdErr)) {
            out.targetUrl = landing;
            out.ok = true;
            return true;
        }
        ZLog(L"[img-search] baidu direct failed (%ls), fallback to imgbb", bdErr.c_str());
    }

    std::wstring key = TrimW(imgbbKey);
    if (key.empty()) {
        out.error = L"未配置 ImgBB API Key";
        return false;
    }

    // 上传至 ImgBB 拿 i.ibb.co 纯净图片直链（10 分钟自动删除）；偶发网络波动兜底备用临时图床
    std::wstring directUrl, upErr;
    if (!UploadToImgBB(bytes, fileName, mime, key, directUrl, upErr)) {
        std::wstring fbErr;
        if (!UploadToLitterboxFallback(bytes, fileName, mime, directUrl, fbErr)) {
            out.error = upErr.empty() ? fbErr : upErr;
            return false;
        }
    }
    out.directImageUrl = directUrl;
    std::wstring enc = UrlEncodeWide(directUrl);

    switch (engine) {
    case IMG_ENGINE_YANDEX:
        out.targetUrl = L"https://yandex.com/images/search?rpt=imageview&url=" + enc;
        break;
    case IMG_ENGINE_GOOGLE:
        out.targetUrl = L"https://lens.google.com/uploadbyurl?url=" + enc;
        break;
    case IMG_ENGINE_BING:
        // 注意：必应国内版 cn.bing.com 已移除以图搜图接口，访问 iss=sbi 会被重定向到“世界名迹/世界旅游胜地”；
        // 必须走国际版 www.bing.com 并携带 form=SBIVSP&sbisrc=UrlPaste
        out.targetUrl =
            L"https://www.bing.com/images/search?view=detailv2&iss=sbi&form=SBIVSP&sbisrc=UrlPaste&q=imgurl:" +
            enc;
        break;
    case IMG_ENGINE_BAIDU:
        out.targetUrl = L"https://graph.baidu.com/details?isfromtusoupc=1&tn=pc&carousel=0&image=" + enc;
        break;
    case IMG_ENGINE_SAUCENAO:
        out.targetUrl = L"https://saucenao.com/search.php?url=" + enc;
        break;
    case IMG_ENGINE_TRACEMOE:
        out.targetUrl = L"https://trace.moe/?url=" + enc;
        break;
    default:
        out.targetUrl = L"https://yandex.com/images/search?rpt=imageview&url=" + enc;
        break;
    }

    out.ok = true;
    return true;
}

}  // namespace img_search
