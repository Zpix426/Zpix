#pragma once
#include <windows.h>
#include <string>

// 全局划词翻译内部消息（投递给主隐藏消息窗 g_msgWnd）
const UINT WM_ZPIX_SELECT_TRIGGER = WM_APP + 23;
const UINT WM_ZPIX_SELECT_READY = WM_APP + 24;
const UINT WM_ZPIX_SELECT_HIDE = WM_APP + 25;

// 启动/停止全局划词监听（低级鼠标钩子）
void StartSelectMonitor(HWND msgWnd);
void StopSelectMonitor();

// 处理由钩子/后台取词线程投递到主消息窗口的消息
void HandleSelectMessage(UINT msg, WPARAM wp, LPARAM lp);

// 立即隐藏划词悬浮条（如进入截图模式时调用）
void HideSelectBar();
