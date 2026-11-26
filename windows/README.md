# Zpix for Windows

基于 **C++17 / Win32 / GDI+** 原生开发的 Windows 客户端源码目录。

## 模块概览

| 文件 | 职责 |
| :--- | :--- |
| `src/main.cpp` | 单实例守护、托盘图标、全局热键与命令行入口 |
| `src/overlay.cpp` | 全虚拟屏捕获、智能窗口/元素框选、标注、取色器与紧凑工具栏 |
| `src/pin.cpp` | 无边框置顶贴图窗口、锚点滚轮缩放与透明度控制 |
| `src/ocr_paddle.cpp` | 本地 PP-OCRv5（ONNX Runtime）离线文字检测、识别与段落聚类 |
| `src/img_translate.cpp` | 图内原地覆盖翻译（背景采样擦除、字号自适应与 ClearType 排版） |
| `src/translate.cpp` | 多引擎并行翻译（BliBli / Google / 智谱 AI / OpenAI 兼容）与批量标记校验 |
| `src/img_search.cpp` | ImgBB 10 分钟临时直链上传与六大识图引擎跳转 |
| `src/select_translate.cpp` | 全局鼠标划词监听与悬浮快捷翻译条 |
| `src/settings.cpp` | 纯白自绘实时自动保存设置面板与多屏不同 DPI 平滑拖拽 |
| `src/setup.cpp` | 单文件自解压现代安装 / 卸载向导（生成 `ZpixSetup.exe`） |

## 构建方法

运行本目录下的 `build.bat`（或在仓库根目录运行 `build.bat`）即可输出 `Zpix.exe` 与 `ZpixSetup.exe`。
