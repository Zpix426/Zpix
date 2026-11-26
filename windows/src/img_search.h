#pragma once
// 以图搜图（识图）：ImgBB 10 分钟自动销毁直链 + 百度直传 + 六大识图引擎跳转
#include "util.h"
#include <string>

enum ImgSearchEngine {
    IMG_ENGINE_YANDEX = 0,
    IMG_ENGINE_GOOGLE = 1,
    IMG_ENGINE_BING = 2,
    IMG_ENGINE_BAIDU = 3,
    IMG_ENGINE_SAUCENAO = 4,
    IMG_ENGINE_TRACEMOE = 5,
    IMG_ENGINE_COUNT = 6
};

namespace img_search {

const wchar_t* EngineLabel(int engine);
const wchar_t* EngineDesc(int engine);

struct SearchResult {
    bool ok = false;
    int engine = 0;
    std::wstring engineName;
    std::wstring directImageUrl;  // 图床直链（如 https://i.ibb.co/...）
    std::wstring targetUrl;       // 最终要在浏览器打开的识图结果页 URL
    std::wstring error;
};

// 同步上传并生成目标识图页面 URL（供后台线程调用）
bool UploadAndBuildSearchUrl(const Bitmap32& img, int engine, const std::wstring& imgbbKey,
                             SearchResult& out);

}  // namespace img_search
