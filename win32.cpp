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

// ---- integrity level / elevated detection (UIPI) ----

// Read the mandatory-integrity SID RID of a process. Returns -1 if unreadable.
static long ProcessIntegrityRid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return -1;
    HANDLE tok = nullptr;
    long rid = -1;
    if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        DWORD size = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &size);
        if (size > 0 && size <= 4096) {
            std::vector<BYTE> buf(size);
            if (GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), size, &size)) {
                // TOKEN_MANDATORY_LABEL: { SID_AND_ATTRIBUTES Label; }
                // Label is a PSID stored at offset sizeof(void*).
                // TOKEN_MANDATORY_LABEL = { SID_AND_ATTRIBUTES Label; } with Label.Sid (PSID) first.
                PSID sid = *reinterpret_cast<PSID*>(buf.data());
                if (sid && IsValidSid(sid)) {
                    DWORD subCount = *GetSidSubAuthorityCount(sid);
                    if (subCount > 0) rid = (long)*GetSidSubAuthority(sid, subCount - 1);
                }
            }
        }
        CloseHandle(tok);
    }
    CloseHandle(h);
    return rid;
}

bool IsElevatedWindow(HWND hwnd) {
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(hwnd, &pid) || pid == 0) return false;
    // SECURITY_MANDATORY_HIGH_RID = 0x00003000
    return ProcessIntegrityRid(pid) >= 0x3000;
}

bool IsSelfElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION elev = {};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &size);
    CloseHandle(tok);
    return ok && elev.TokenIsElevated != 0;
}

bool ForegroundWindowInfo(TopWindowInfo& out) {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return false;
    out.hwnd = hwnd;
    out.class_name = ClassNameOf(hwnd);
    out.title = TitleOf(hwnd);
    GetWindowThreadProcessId(hwnd, &out.pid);
    return true;
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

// button: "left"|"right"|"middle"; count: 1 or 2 (double-click)
bool MouseClick(long x, long y, const char* button, int count) {
    if (!MouseMove(x, y)) return false;
    Sleep(30);
    DWORD downFlag = MOUSEEVENTF_LEFTDOWN;
    DWORD upFlag = MOUSEEVENTF_LEFTUP;
    if (button && _stricmp(button, "right") == 0) {
        downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag = MOUSEEVENTF_RIGHTUP;
    } else if (button && _stricmp(button, "middle") == 0) {
        downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP;
    }
    if (count < 1) count = 1;
    if (count > 2) count = 2;
    for (int i = 0; i < count; ++i) {
        INPUT down = {};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = downFlag;
        if (SendInput(1, &down, sizeof(INPUT)) != 1) return false;
        Sleep(30);
        INPUT up = {};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = upFlag;
        if (SendInput(1, &up, sizeof(INPUT)) != 1) return false;
        if (count == 2 && i == 0) Sleep(50); // double-click pause
    }
    return true;
}

bool MouseLeftClick(long x, long y) { return MouseClick(x, y, "left", 1); }

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

// 组合键解析："ctrl+c" / "alt+tab" / "shift+f10" / "win+e"。修饰键支持 ctrl/alt/shift/win。
bool SendKeys(const std::wstring& combo) {
    // tokenize by '+'
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= combo.size()) {
        size_t pos = combo.find(L'+', start);
        std::wstring tok = combo.substr(start, pos == std::wstring::npos ? std::wstring::npos : pos - start);
        for (auto& ch : tok) if (ch >= L'a' && ch <= L'z') ch = ch - L'a' + L'A';
        if (!tok.empty()) parts.push_back(tok);
        if (pos == std::wstring::npos) break;
        start = pos + 1;
    }
    // map tokens to VK codes
    std::vector<WORD> mods, keys;
    for (const auto& t : parts) {
        if (t == L"CTRL" || t == L"CONTROL") mods.push_back(VK_CONTROL);
        else if (t == L"ALT") mods.push_back(VK_MENU);
        else if (t == L"SHIFT") mods.push_back(VK_SHIFT);
        else if (t == L"WIN") mods.push_back(VK_LWIN);
        else if (t == L"ENTER" || t == L"RETURN") keys.push_back(VK_RETURN);
        else if (t == L"TAB") keys.push_back(VK_TAB);
        else if (t == L"ESC" || t == L"ESCAPE") keys.push_back(VK_ESCAPE);
        else if (t == L"SPACE") keys.push_back(VK_SPACE);
        else if (t == L"BACKSPACE") keys.push_back(VK_BACK);
        else if (t == L"DELETE") keys.push_back(VK_DELETE);
        else if (t == L"INSERT") keys.push_back(VK_INSERT);
        else if (t == L"HOME") keys.push_back(VK_HOME);
        else if (t == L"END") keys.push_back(VK_END);
        else if (t == L"PAGEUP") keys.push_back(VK_PRIOR);
        else if (t == L"PAGEDOWN") keys.push_back(VK_NEXT);
        else if (t == L"UP") keys.push_back(VK_UP);
        else if (t == L"DOWN") keys.push_back(VK_DOWN);
        else if (t == L"LEFT") keys.push_back(VK_LEFT);
        else if (t == L"RIGHT") keys.push_back(VK_RIGHT);
        else if (t == L"F1") keys.push_back(VK_F1);
        else if (t == L"F2") keys.push_back(VK_F2);
        else if (t == L"F3") keys.push_back(VK_F3);
        else if (t == L"F4") keys.push_back(VK_F4);
        else if (t == L"F5") keys.push_back(VK_F5);
        else if (t == L"F6") keys.push_back(VK_F6);
        else if (t == L"F7") keys.push_back(VK_F7);
        else if (t == L"F8") keys.push_back(VK_F8);
        else if (t == L"F9") keys.push_back(VK_F9);
        else if (t == L"F10") keys.push_back(VK_F10);
        else if (t == L"F11") keys.push_back(VK_F11);
        else if (t == L"F12") keys.push_back(VK_F12);
        else if (t.size() == 1) keys.push_back(static_cast<WORD>(t[0])); // single char
        else return false; // unknown key
    }
    if (keys.empty()) return false;
    // send: mods down, key down/up, mods up
    std::vector<INPUT> in;
    auto press = [&](WORD vk, bool down) {
        INPUT x = {};
        x.type = INPUT_KEYBOARD;
        x.ki.wVk = vk;
        if (!down) x.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(x);
    };
    for (WORD m : mods) press(m, true);
    for (WORD k : keys) press(k, true);
    for (WORD k : keys) press(k, false);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) press(*it, false);
    UINT sent = SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
    return sent == in.size();
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


// ---- clipboard ----
bool ClipboardGetText(std::wstring& out) {
    out.clear();
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
        if (p) {
            out = p;
            GlobalUnlock(h);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

bool ClipboardSetText(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != FALSE;
            if (!ok) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
}


// ---- 窗口几何 ----
bool GetWindowGeometry(HWND hwnd, long& x, long& y, long& w, long& h) {
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    x = r.left; y = r.top;
    w = r.right - r.left; h = r.bottom - r.top;
    return true;
}

bool SetWindowGeometry(HWND hwnd, long x, long y, long w, long h) {
    if (w <= 0 || h <= 0) return false;
    return SetWindowPos(hwnd, nullptr, static_cast<int>(x), static_cast<int>(y),
                        static_cast<int>(w), static_cast<int>(h),
                        SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

// ---- 桌面图标宿主 ----
// 找 WorkerW 且其子窗口含 SHELLDLL_DefView（桌面图标在 SysListView32 里）。
static BOOL CALLBACK DesktopEnumProc(HWND hwnd, LPARAM lp) {
    HWND* out = reinterpret_cast<HWND*>(lp);
    wchar_t cls[128];
    if (GetClassNameW(hwnd, cls, 128) > 0 && _wcsicmp(cls, L"WorkerW") == 0) {
        // WorkerW 的子窗口 SHELLDLL_DefView 下有 SysListView32
        HWND defview = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defview) {
            HWND listview = FindWindowExW(defview, nullptr, L"SysListView32", nullptr);
            if (listview) {
                *out = hwnd; // 带桌面图标的 WorkerW
                return FALSE; // 停止枚举
            }
        }
    }
    return TRUE;
}

HWND FindDesktopIconHost() {
    // 布局 A（Win7 传统）：Progman 直接挂 SHELLDLL_DefView
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND dv = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (dv) {
            HWND lv = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
            if (lv) return progman;
        }
    }
    // 布局 B（Win8+）：WorkerW 包一层
    HWND h = nullptr;
    EnumWindows(DesktopEnumProc, reinterpret_cast<LPARAM>(&h));
    if (h) return h;
    // 兜底：Progman 本身（即使无 DefView 也返回，让调用方尝试）
    return progman;
}

} // namespace win32