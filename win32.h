#pragma once
// Win32 封装：Z-order 枚举、子窗口遍历、SendInput 鼠标/键盘。
#include <atomic>
#include <string>
#include <vector>
#include <windows.h>
#include "uia.h"  // UiNode / UiRect

namespace win32 {

struct TopWindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring class_name;
    DWORD pid = 0;
};

// 真实 Z-order（最前 -> 最后）：EnumWindows 取最上层 + GetWindow(GW_HWNDNEXT)。
std::vector<HWND> TopZOrder();

// 可见顶层窗口，过滤 #32768/Tooltip/SysShadow/MSCTFIME UI/IME 类名前缀。
std::vector<TopWindowInfo> ListTopWindows();

std::wstring ClassNameOf(HWND hwnd);
std::wstring TitleOf(HWND hwnd);
bool IsFilteredClass(const std::wstring& cls);

// 目标窗口是否属于 elevated（高完整性）进程：读其 TokenIntegrityLevel >= High。
// 非管理员进程访问 elevated 窗口会被 UIPI 拦截（UIA 空树 / SendInput 被吞）。
bool IsElevatedWindow(HWND hwnd);
// 当前进程是否以管理员（elevated）运行。
bool IsSelfElevated();

// 当前拥有键盘焦点的顶层窗口信息（GetForegroundWindow）。
bool ForegroundWindowInfo(TopWindowInfo& out);

// Win32 子窗口树（snapshot_all 降级后端）。返回节点数；root 始终 >= 1（含根）。
int BuildChildTree(HWND hwnd, int maxDepth, int maxNodes,
                   std::atomic<bool>* cancel, UiNode& root);

// ---- 输入（全部走 SendInput，物理像素）----
bool MouseMove(long x, long y);
bool MouseLeftClick(long x, long y);   // 移动 + 按下 + 抬起（左键单击）
bool MouseClick(long x, long y, const char* button, int count); // button=left/right/middle, count=1/2
bool MouseWheelAt(long x, long y, long delta); // positive delta = 滚轮向后(向上滚)
bool DragPath(long x1, long y1, long x2, long y2, int steps, DWORD durationMs);
bool TypeUnicode(const std::wstring& text);
bool SendCtrlA();
void ForceForeground(HWND hwnd);

// ---- 窗口状态（ShowWindow / SetWindowPos）----
bool SetWindowTopmost(HWND hwnd, bool topmost); // WS_EX_TOPMOST 置顶 / 取消置顶
bool MinimizeWindow(HWND hwnd);                 // SW_MINIMIZE
bool MaximizeWindow(HWND hwnd);                 // SW_MAXIMIZE
bool RestoreWindow(HWND hwnd);                  // SW_RESTORE
bool CloseWindow(HWND hwnd);                    // 发送 WM_CLOSE（应用可拦截/保存）

} // namespace win32