#include "win32.h"
#include <windows.h>
#include <cstdlib>
#include <cwchar>
#include "util.h"

namespace win32 {

namespace {

HWND g_firstTop = nullptr;

BOOL CALLBACK FirstEnumProc(HWND hwnd, LPARAM) {
    g_firstTop = hwnd; // 第一个 = Z-order 最前
    return FALSE;
}

const wchar_t* const kFilterPrefixes[] = {
    L"#32768", L"Tooltip", L"SysShadow", L"MSCTFIME UI", L"IME"};

} // namespace

std::wstring ClassNameOf(HWND hwnd) {
    wchar_t buf[512];
    int n = GetClassNameW(hwnd, buf, static_cast<int>(_countof(buf)));
    return (n > 0) ? std::wstring(buf, n) : std::wstring();
}

std::wstring TitleOf(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    int n = GetWindowTextW(hwnd, s.data(), len + 1);
    if (n > 0) s.resize(n); else s.clear();
    return s;
}

bool IsFilteredClass(const std::wstring& cls) {
    for (auto* p : kFilterPrefixes) {
        if (_wcsnicmp(cls.c_str(), p, wcslen(p)) == 0) return true;
    }
    return false;
}

std::vector<HWND> TopZOrder() {
    g_firstTop = nullptr;
    EnumWindows(FirstEnumProc, 0); // 拿到最上层窗口句柄
    std::vector<HWND> out;
    for (HWND h = g_firstTop; h; h = GetWindow(h, GW_HWNDNEXT)) out.push_back(h);
    return out;
}

std::vector<TopWindowInfo> ListTopWindows() {
    std::vector<TopWindowInfo> out;
    for (HWND h : TopZOrder()) {
        if (!IsWindowVisible(h)) continue;
        std::wstring cls = ClassNameOf(h);
        if (IsFilteredClass(cls)) continue;
        TopWindowInfo info;
        info.hwnd = h;
        info.class_name = cls;
        info.title = TitleOf(h);
        GetWindowThreadProcessId(h, &info.pid);
        out.push_back(std::move(info));
    }
    return out;
}

// ---- Win32 子窗口树（snapshot_all 降级后端）----
namespace {

void FillWin32Node(HWND hwnd, UiNode& n) {
    n.name = WideToUtf8(TitleOf(hwnd));
    n.class_name = WideToUtf8(ClassNameOf(hwnd));
    n.control_type = "Window";
    RECT r{};
    if (GetWindowRect(hwnd, &r)) {
        n.rect.left = r.left; n.rect.top = r.top;
        n.rect.right = r.right; n.rect.bottom = r.bottom;
    }
}

void CollectWin32Children(HWND parent, int depth, int maxDepth, int maxNodes,
                          std::atomic<bool>* cancel, int& count, UiNode& out) {
    if (depth >= maxDepth) return;
    if (cancel && cancel->load()) return;
    HWND child = GetWindow(parent, GW_CHILD);
    while (child) {
        if (count >= maxNodes) break;
        if (cancel && cancel->load()) return;
        ++count;
        UiNode c;
        FillWin32Node(child, c);
        CollectWin32Children(child, depth + 1, maxDepth, maxNodes, cancel, count, c);
        out.children.push_back(std::move(c));
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

} // namespace

int BuildChildTree(HWND hwnd, int maxDepth, int maxNodes,
                   std::atomic<bool>* cancel, UiNode& root) {
    int count = 1;
    FillWin32Node(hwnd, root);
    if (maxDepth > 0) {
        CollectWin32Children(hwnd, 1, maxDepth, maxNodes, cancel, count, root);
    }
    return count;
}

// ---- SendInput（物理像素，绝对坐标归一化到主屏 0..65535）----
namespace {

LONG NormX(long x) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    if (sw <= 1) return 0;
    if (x < 0) x = 0;
    if (x >= sw) x = sw - 1;
    return static_cast<LONG>((static_cast<__int64>(x) * 65535L) / (sw - 1));
}

LONG NormY(long y) {
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (sh <= 1) return 0;
    if (y < 0) y = 0;
    if (y >= sh) y = sh - 1;
    return static_cast<LONG>((static_cast<__int64>(y) * 65535L) / (sh - 1));
}

INPUT MouseMoveInput(long x, long y) {
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = NormX(x);
    in.mi.dy = NormY(y);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    return in;
}

} // namespace

bool MouseMove(long x, long y) {
    INPUT in = MouseMoveInput(x, y);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool MouseLeftClick(long x, long y) {
    if (!MouseMove(x, y)) return false;
    Sleep(30);
    INPUT down = {};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    bool ok = SendInput(1, &down, sizeof(INPUT)) == 1;
    Sleep(30);
    INPUT up = {};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    ok = SendInput(1, &up, sizeof(INPUT)) == 1 && ok;
    return ok;
}

bool MouseWheelAt(long x, long y, long delta) {
    if (!MouseMove(x, y)) return false;
    Sleep(30);
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(delta); // WHEEL_DELTA=120
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool DragPath(long x1, long y1, long x2, long y2, int steps, DWORD durationMs) {
    if (!MouseMove(x1, y1)) return false;
    Sleep(40);
    INPUT down = {};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    if (SendInput(1, &down, sizeof(INPUT)) != 1) return false;
    Sleep(60);
    if (steps < 2) steps = 2;
    DWORD stepMs = durationMs / static_cast<DWORD>(steps);
    for (int i = 1; i <= steps; ++i) {
        double t = static_cast<double>(i) / steps;
        long x = static_cast<long>(x1 + (x2 - x1) * t);
        long y = static_cast<long>(y1 + (y2 - y1) * t);
        MouseMove(x, y);
        if (stepMs) Sleep(stepMs);
    }
    MouseMove(x2, y2);
    Sleep(30);
    INPUT up = {};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    return SendInput(1, &up, sizeof(INPUT)) == 1;
}

bool TypeUnicode(const std::wstring& text) {
    for (wchar_t wc : text) {
        INPUT keys[2] = {};
        keys[0].type = INPUT_KEYBOARD;
        keys[0].ki.wVk = 0;
        keys[0].ki.wScan = static_cast<WORD>(wc);
        keys[0].ki.dwFlags = KEYEVENTF_UNICODE;
        keys[1] = keys[0];
        keys[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        if (SendInput(2, keys, sizeof(INPUT)) != 2) return false;
        Sleep(8);
    }
    return true;
}

bool SendCtrlA() {
    INPUT keys[4] = {};
    keys[0].type = INPUT_KEYBOARD; keys[0].ki.wVk = VK_CONTROL;
    keys[1].type = INPUT_KEYBOARD; keys[1].ki.wVk = 'A';
    keys[2] = keys[1]; keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3] = keys[0]; keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    bool ok = SendInput(4, keys, sizeof(INPUT)) == 4;
    Sleep(40);
    return ok;
}

// ---- 窗口状态 ----
bool SetWindowTopmost(HWND hwnd, bool topmost) {
    HWND after = topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
    return SetWindowPos(hwnd, after, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
}

bool MinimizeWindow(HWND hwnd) { return ShowWindow(hwnd, SW_MINIMIZE) != FALSE; }
bool MaximizeWindow(HWND hwnd) { return ShowWindow(hwnd, SW_MAXIMIZE) != FALSE; }
bool RestoreWindow(HWND hwnd)  { return ShowWindow(hwnd, SW_RESTORE) != FALSE; }
bool CloseWindow(HWND hwnd)    { return PostMessage(hwnd, WM_CLOSE, 0, 0) != FALSE; }

void ForceForeground(HWND hwnd) {
    if (!hwnd) return;
    DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
    bool attached = false;
    if (fgThread && targetThread && fgThread != targetThread) {
        // 解除前台锁定：附加线程输入后 SetForegroundWindow 更可靠
        attached = AttachThreadInput(fgThread, targetThread, TRUE) != FALSE;
        if (attached) {
            SetForegroundWindow(hwnd);
            AttachThreadInput(fgThread, targetThread, FALSE);
        }
    }
    if (!attached) {
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        // 直接 SetForeground 被锁时，模拟一次 Alt 键解锁前台
        INPUT alt = {};
        alt.type = INPUT_KEYBOARD;
        alt.ki.wVk = VK_MENU;
        SendInput(1, &alt, sizeof(INPUT));
        INPUT altUp = alt;
        altUp.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &altUp, sizeof(INPUT));
        SetForegroundWindow(hwnd);
    }
    Sleep(60);
}

} // namespace win32