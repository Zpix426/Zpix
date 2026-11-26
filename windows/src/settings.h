#pragma once
#include <windows.h>

enum SettingsTab {
    SETTINGS_TAB_HOTKEY = 0,
    SETTINGS_TAB_TRANSLATE = 1,
    SETTINGS_TAB_IMGSEARCH = 2,
    SETTINGS_TAB_SAVE = 3,
};

// 打开设置窗口（initialTab >= 0 时直接切换到指定标签页）
void ShowSettingsWindow(HWND owner, int initialTab = -1);
bool IsSettingsOpen();
