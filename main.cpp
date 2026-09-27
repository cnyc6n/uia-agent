// uia_agent - Windows UI Automation agent (single exe).
// 入口：参数解析 / 命令分发 / 超时线程模型 / UTF-8 JSON 输出。
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <objbase.h>  // WIN32_LEAN_AND_MEAN 裁掉 COM 头，需显式引入

#pragma warning(push, 0)  // 第三方头不刷屏
#include <json.hpp>
#pragma warning(pop)

#include "capture.h"
#include "uia.h"
#include "util.h"
#include "win32.h"

using json = nlohmann::json;

namespace {

constexpr int kDefaultTimeoutMs = 5000;
constexpr int kMaxNodes = 5000;

// ================= 参数解析 =================

struct ParsedArgs {
    std::string cmd;
    std::vector<std::pair<std::string, std::string>> kv;
    std::set<std::string> flags;

    // 键统一不含 "--" 前缀（解析时已剥离），查询时容忍带前缀写法
    static std::string NormKey(const std::string& key) {
        if (key.rfind("--", 0) == 0) return key.substr(2);
        return key;
    }
    std::string get(const std::string& key, const std::string& def = "") const {
        std::string k = NormKey(key);
        for (const auto& p : kv) if (p.first == k) return p.second;
        return def;
    }
    bool hasFlag(const std::string& key) const { return flags.count(NormKey(key)) != 0; }
    bool hasKey(const std::string& key) const { return has(NormKey(key)); }
    bool has(const std::string& k) const {
        for (const auto& p : kv) if (p.first == k) return true;
        return false;
    }
    bool getLL(const std::string& key, long long& out) const {
        const std::string v = get(key);
        if (v.empty()) return false;
        char* end = nullptr;
        errno = 0;
        out = strtoll(v.c_str(), &end, 0); // 0: 自动识别 10/16 进制前缀
        return end && *end == '\0';
    }
    bool getInt(const std::string& key, int& out) const {
        long long v;
        if (!getLL(key, v)) return false;
        out = static_cast<int>(v);
        return true;
    }
};

bool ParseArgs(int argc, wchar_t** argv, ParsedArgs& out) {
    std::vector<std::string> tokens;
    for (int i = 1; i < argc; ++i) {
        std::wstring w(argv[i]);
        tokens.push_back(WideToUtf8(w));
    }
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t.rfind("--", 0) == 0) {
            std::string key, val;
            bool hasVal = false;
            size_t eq = t.find('=');
            if (eq != std::string::npos) {
                key = t.substr(2, eq - 2);
                val = t.substr(eq + 1);
                hasVal = true;
            } else {
                key = t.substr(2);
                if (i + 1 < tokens.size() && tokens[i + 1].rfind("--", 0) != 0) {
                    val = tokens[++i];
                    hasVal = true;
                }
            }
            if (!key.empty()) {
                if (hasVal) {
                    // 同名 kv 仅保留第一个
                    bool dup = false;
                    for (const auto& p : out.kv) if (p.first == key) { dup = true; break; }
                    if (!dup) out.kv.emplace_back(key, val);
                } else {
                    out.flags.insert(key);
                }
            }
        } else if (out.cmd.empty()) {
            out.cmd = t;
        } else {
            return false; // 多余的位置参数
        }
    }
    return !out.cmd.empty() || !out.flags.empty();
}

// ================= 输出助手 =================

std::string ErrStr(const std::string& e) {
    json j;
    j["ok"] = false;
    j["err"] = e;
    return j.dump();
}

std::string OkStr(json body) {
    body["ok"] = true;
    return body.dump();
}

json RectToJson(const UiRect& r) {
    json j;
    j["left"] = r.left;
    j["top"] = r.top;
    j["right"] = r.right;
    j["bottom"] = r.bottom;
    j["cx"] = r.cx();
    j["cy"] = r.cy();
    return j;
}

json NodeToJson(const UiNode& n, bool withChildren) {
    json j;
    j["name"] = n.name;
    j["control_type"] = n.control_type;
    j["automation_id"] = n.automation_id;
    j["class_name"] = n.class_name;
    j["rect"] = RectToJson(n.rect);
    if (withChildren) {
        json arr = json::array();
        for (const auto& c : n.children) arr.push_back(NodeToJson(c, true));
        j["children"] = arr;
    } else {
        j["children"] = json::array();
    }
    return j;
}

HWND HwndFromLL(long long v) { return reinterpret_cast<HWND>(static_cast<intptr_t>(v)); }

bool GetHwnd(const ParsedArgs& a, HWND& out) {
    long long v;
    if (!a.getLL("--hwnd", v)) return false;
    out = HwndFromLL(v);
    return true;
}

int DepthFromArgs(const ParsedArgs& a, int defaultDepth) {
    int d = defaultDepth;
    if (a.getInt("--depth", d)) { /* parsed */ }
    if (d < 1) d = 1;
    if (d > 64) d = 64;
    return d;
}

// 解析 --q 查询条件
bool ParseFilter(const ParsedArgs& a, QueryFilter& f, std::string& err) {
    std::string q = a.get("--q");
    if (q.empty()) { err = "missing_arg_--q"; return false; }
    try {
        json j = json::parse(q);
        if (!j.is_object()) { err = "query_not_object"; return false; }
        auto take = [&](const char* key, bool& has, std::string& val) {
            auto it = j.find(key);
            if (it != j.end() && it->is_string()) { has = true; val = it->get<std::string>(); }
        };
        take("name", f.has_name, f.name);
        take("automation_id", f.has_aid, f.automation_id);
        take("control_type", f.has_type, f.control_type);
        take("class_name", f.has_class, f.class_name);
        return true;
    } catch (const json::exception& e) {
        err = std::string("bad_query:") + e.what();
        return false;
    }
}

// ================= 命令实现 =================


// 若目标窗口属于 elevated 进程而自身非管理员，返回错误 JSON；否则返回空串。
// UIPI 会拦截跨完整性级别的 UIA/消息，明确报错比返回空树更有用。
std::string ElevatedGuard(HWND hwnd) {
    if (win32::IsElevatedWindow(hwnd) && !win32::IsSelfElevated()) {
        return ErrStr("elevated_requires_admin");
    }
    return std::string();
}

std::string CmdList(const ParsedArgs&) {
    json arr = json::array();
    for (const auto& w : win32::ListTopWindows()) {
        json item;
        item["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(w.hwnd));
        item["title"] = WideToUtf8(w.title);
        item["class_name"] = WideToUtf8(w.class_name);
        item["pid"] = static_cast<long long>(w.pid);
        item["elevated"] = win32::IsElevatedWindow(w.hwnd);
        arr.push_back(std::move(item));
    }
    return arr.dump(); // 按 spec：list 返回裸数组
}

std::string CmdForeground(const ParsedArgs&) {
    win32::TopWindowInfo w;
    if (!win32::ForegroundWindowInfo(w)) return ErrStr("no_foreground_window");
    json item;
    item["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(w.hwnd));
    item["title"] = WideToUtf8(w.title);
    item["class_name"] = WideToUtf8(w.class_name);
    item["pid"] = static_cast<long long>(w.pid);
    item["elevated"] = win32::IsElevatedWindow(w.hwnd);
    return item.dump();
}

std::string CmdSnapshot(const ParsedArgs& a, std::atomic<bool>* cancel) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    int depth = DepthFromArgs(a, 8);
    UiNode root;
    int n = UiaBuildTree(hwnd, depth, kMaxNodes, cancel, root);
    if (n == 0) return ErrStr("unsupported");
    json body;
    body["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(hwnd));
    body["depth"] = depth;
    body["nodes"] = n;
    body["tree"] = NodeToJson(root, true);
    return OkStr(std::move(body));
}

std::string CmdSnapshotAll(const ParsedArgs& a, std::atomic<bool>* cancel) {
    int depth = DepthFromArgs(a, 8);
    json wins = json::array();
    for (const auto& w : win32::ListTopWindows()) {
        json item;
        item["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(w.hwnd));
        item["title"] = WideToUtf8(w.title);
        item["class_name"] = WideToUtf8(w.class_name);
        bool isElev = win32::IsElevatedWindow(w.hwnd);
        item["elevated"] = isElev;

        json states = json::array();
        if (IsIconic(w.hwnd)) states.push_back("minimized");
        if (!IsWindowVisible(w.hwnd)) states.push_back("invisible");
        if (isElev && !win32::IsSelfElevated()) states.push_back("elevated");

        std::string backend = "uia";
        UiNode tree;
        int n = 0;
        if (!IsWindow(w.hwnd)) {
            backend = "none";
            states.push_back("unsupported");
        } else {
            n = UiaBuildTree(w.hwnd, depth, kMaxNodes, cancel, tree);
            if (n <= 1) { // UIA 只有根 → 降级 Win32 枚举
                backend = "win32";
                UiNode t2;
                int n2 = win32::BuildChildTree(w.hwnd, depth, kMaxNodes, cancel, t2);
                if (n2 > 0) {
                    tree = std::move(t2);
                    n = n2;
                } else {
                    backend = "none";
                    states.push_back("unsupported");
                }
            }
        }
        item["backend"] = backend;
        item["state"] = std::move(states);
        item["tree"] = (n > 0) ? NodeToJson(tree, true) : json::object();
        wins.push_back(std::move(item));
    }
    json body;
    body["windows"] = std::move(wins);
    return OkStr(std::move(body));
}

std::string CmdFind(const ParsedArgs& a, std::atomic<bool>* cancel) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    QueryFilter f;
    std::string err;
    if (!ParseFilter(a, f, err)) return ErrStr(err);
    std::vector<UiaHit> hits;
    int walked = UiaFindElements(hwnd, f, kMaxNodes, cancel, hits);
    json arr = json::array();
    for (const auto& h : hits) arr.push_back(NodeToJson(h.info, false));
    json body;
    body["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(hwnd));
    body["count"] = static_cast<int>(hits.size());
    body["walked"] = walked;
    body["nodes"] = std::move(arr);
    return OkStr(std::move(body));
}

std::string CmdClick(const ParsedArgs& a, std::atomic<bool>* cancel) {
    long long x = 0, y = 0;
    bool hasXY = a.getLL("--x", x) && a.getLL("--y", y);
    if (hasXY) {
        std::string btn = a.get("--button", "left");
        int cnt = 1;
        a.getInt("--count", cnt);
        if (!win32::MouseClick(static_cast<long>(x), static_cast<long>(y), btn.c_str(), cnt))
            return ErrStr("send_input_failed");
        json body;
        body["mode"] = "coordinate";
        body["button"] = btn;
        body["count"] = cnt;
        body["x"] = x;
        body["y"] = y;
        return OkStr(std::move(body));
    }
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    QueryFilter f;
    std::string err;
    if (!ParseFilter(a, f, err)) return ErrStr(err);
    std::string mode = a.get("--mode", "semantic");

    std::vector<UiaHit> hits;
    UiaFindElements(hwnd, f, kMaxNodes, cancel, hits);
    if (hits.empty()) return ErrStr("not_found");
    const UiaHit& hit = hits[0];

    if (mode != "mouse" && UiaInvoke(hit)) {
        json body;
        body["mode"] = "semantic";
        body["method"] = "invoke_pattern";
        body["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(hwnd));
        return OkStr(std::move(body));
    }
    // 退化：rect 中心 SendInput
    const UiRect& r = hit.info.rect;
    if (r.cx() <= 0 || r.cy() <= 0) return ErrStr("no_rect");
    long cx = r.left + r.cx() / 2;
    long cy = r.top + r.cy() / 2;
    std::string btn = a.get("--button", "left");
    int cnt = 1;
    a.getInt("--count", cnt);
    win32::ForceForeground(hwnd);
    if (!win32::MouseClick(cx, cy, btn.c_str(), cnt)) return ErrStr("send_input_failed");
    json body;
    body["mode"] = mode;
    body["method"] = "mouse_click";
    body["button"] = btn;
    body["count"] = cnt;
    body["x"] = cx;
    body["y"] = cy;
    return OkStr(std::move(body));
}

std::string CmdSetText(const ParsedArgs& a, std::atomic<bool>* cancel) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    std::string text = a.get("--text");
    if (text.empty()) return ErrStr("missing_arg_--text");
    QueryFilter f;
    std::string err;
    if (!ParseFilter(a, f, err)) return ErrStr(err);

    std::vector<UiaHit> hits;
    UiaFindElements(hwnd, f, kMaxNodes, cancel, hits);
    if (hits.empty()) return ErrStr("not_found");
    const UiaHit& hit = hits[0];

    // 先试 ValuePattern::SetValue
    if (UiaValuePatternSet(hit, text)) {
        json body;
        body["method"] = "value_pattern";
        body["text"] = text;
        return OkStr(std::move(body));
    }
    // 退化：点击 + Ctrl+A + 键入
    const UiRect& r = hit.info.rect;
    if (r.cx() <= 0 || r.cy() <= 0) return ErrStr("no_rect");
    long cx = r.left + r.cx() / 2;
    long cy = r.top + r.cy() / 2;
    win32::ForceForeground(hwnd);
    if (!win32::MouseLeftClick(cx, cy)) return ErrStr("send_input_failed");
    Sleep(80);
    win32::SendCtrlA();
    Sleep(50);
    if (!win32::TypeUnicode(Utf8ToWide(text))) return ErrStr("send_input_failed");
    json body;
    body["method"] = "input_simulation";
    return OkStr(std::move(body));
}

std::string CmdGetText(const ParsedArgs& a, std::atomic<bool>* cancel) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    QueryFilter f;
    std::string err;
    if (!ParseFilter(a, f, err)) return ErrStr(err);

    std::vector<UiaHit> hits;
    UiaFindElements(hwnd, f, kMaxNodes, cancel, hits);
    if (hits.empty()) return ErrStr("not_found");
    std::string text;
    if (!UiaValuePatternGet(hits[0], text)) return ErrStr("no_value_pattern");
    json body;
    body["text"] = text;
    return OkStr(std::move(body));
}

std::string CmdScroll(const ParsedArgs& a, std::atomic<bool>* cancel) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    long long amount = 1;
    a.getLL("--amount", amount);

    UiRect rect;
    bool usedPattern = false;
    if (a.hasKey("--q")) {
        QueryFilter f;
        std::string err;
        if (!ParseFilter(a, f, err)) return ErrStr(err);
        std::vector<UiaHit> hits;
        UiaFindElements(hwnd, f, kMaxNodes, cancel, hits);
        if (hits.empty()) return ErrStr("not_found");
        const UiaHit& hit = hits[0];
        rect = hit.info.rect;
        if (UiaScrollViaPattern(hit, static_cast<int>(amount)) == 1) usedPattern = true;
    } else {
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return ErrStr("bad_window_rect");
        rect.left = r.left; rect.top = r.top; rect.right = r.right; rect.bottom = r.bottom;
    }

    if (usedPattern) {
        json body;
        body["method"] = "scroll_pattern";
        body["amount"] = amount;
        return OkStr(std::move(body));
    }
    if (rect.cx() <= 0 || rect.cy() <= 0) return ErrStr("no_rect");
    long cx = rect.left + rect.cx() / 2;
    long cy = rect.top + rect.cy() / 2;
    // amount>0 向下（滚轮 delta 为负），amount<0 向上
    long delta = (amount > 0) ? -static_cast<long>(amount) * 120
                              : static_cast<long>(-amount) * 120;
    if (!win32::MouseWheelAt(cx, cy, delta)) return ErrStr("send_input_failed");
    json body;
    body["method"] = "mouse_wheel";
    body["amount"] = amount;
    return OkStr(std::move(body));
}

std::string CmdDrag(const ParsedArgs& a) {
    long long x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (!(a.getLL("--x1", x1) && a.getLL("--y1", y1) &&
          a.getLL("--x2", x2) && a.getLL("--y2", y2)))
        return ErrStr("need_--x1_--y1_--x2_--y2");
    int duration = 300;
    a.getInt("--duration", duration);
    if (duration < 0) duration = 0;
    int steps = static_cast<int>(duration / 20);
    if (steps < 2) steps = 2;
    if (steps > 60) steps = 60;
    if (!win32::DragPath(static_cast<long>(x1), static_cast<long>(y1),
                         static_cast<long>(x2), static_cast<long>(y2),
                         steps, static_cast<DWORD>(duration)))
        return ErrStr("send_input_failed");
    json body;
    body["from"] = json::array({x1, y1});
    body["to"] = json::array({x2, y2});
    body["duration"] = duration;
    return OkStr(std::move(body));
}

std::string CmdSwipe(const ParsedArgs& a, std::atomic<bool>* cancel) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    std::string dir = a.get("--direction");
    UiRect rect;
    if (a.hasKey("--q")) {
        QueryFilter f;
        std::string err;
        if (!ParseFilter(a, f, err)) return ErrStr(err);
        std::vector<UiaHit> hits;
        UiaFindElements(hwnd, f, kMaxNodes, cancel, hits);
        if (hits.empty()) return ErrStr("not_found");
        rect = hits[0].info.rect;
    } else {
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) return ErrStr("bad_window_rect");
        rect.left = r.left; rect.top = r.top; rect.right = r.right; rect.bottom = r.bottom;
    }
    if (rect.cx() <= 0 || rect.cy() <= 0) return ErrStr("no_rect");

    int distance = 0;
    a.getInt("--distance", distance);
    if (distance <= 0) {
        distance = (dir == "up" || dir == "down") ? rect.cy() / 2 : rect.cx() / 2;
        if (distance <= 0) distance = 10;
    }

    long cx = rect.left + rect.cx() / 2;
    long cy = rect.top + rect.cy() / 2;
    long sx = cx, sy = cy, ex = cx, ey = cy;
    if (dir == "up")   { sy = cy + distance / 2; ey = cy - distance / 2; }
    else if (dir == "down") { sy = cy - distance / 2; ey = cy + distance / 2; }
    else if (dir == "left") { sx = cx + distance / 2; ex = cx - distance / 2; }
    else if (dir == "right"){ sx = cx - distance / 2; ex = cx + distance / 2; }
    else return ErrStr("bad_direction");

    if (!win32::DragPath(sx, sy, ex, ey, 20, 300)) return ErrStr("send_input_failed");
    json body;
    body["direction"] = dir;
    body["distance"] = distance;
    body["start"] = json::array({sx, sy});
    body["end"] = json::array({ex, ey});
    return OkStr(std::move(body));
}

std::string CmdScreenshot(const ParsedArgs& a) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    cap::Shot shot = cap::CaptureWindow(hwnd);
    if (!shot.ok) return ErrStr(shot.err.empty() ? "capture_failed" : shot.err);

    std::string outPath = a.get("--out");
    json body;
    body["w"] = shot.w;
    body["h"] = shot.h;

    if (!outPath.empty()) {
        std::wstring wpath = Utf8ToWide(outPath);
        std::ofstream f(wpath, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(shot.png.data(), static_cast<std::streamsize>(shot.png.size()))) {
            return ErrStr("write_failed");
        }
        body["path"] = outPath;
    } else {
        // 未给 --out 时默认输出 base64
        body["base64"] = cap::Base64Encode(shot.png.data(), shot.png.size());
    }
    return OkStr(std::move(body));
}

// 窗口状态：topmost / minimize / maximize / restore / close
// 用法: <cmd> --hwnd <n> [--on|--off]   (topmost 默认 --on)
std::string CmdWindowState(const ParsedArgs& a) {
    HWND hwnd;
    if (!GetHwnd(a, hwnd)) return ErrStr("missing_arg_--hwnd");
    { std::string eg = ElevatedGuard(hwnd); if (!eg.empty()) return eg; }
    if (!IsWindow(hwnd)) return ErrStr("invalid_window");

    const std::string& c = a.cmd;
    bool ok = false;
    if (c == "topmost") {
        bool on = !a.hasFlag("off");
        ok = win32::SetWindowTopmost(hwnd, on);
        if (!ok) return ErrStr("set_window_pos_failed");
        json body;
        body["state"] = on ? "topmost" : "normal";
        body["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(hwnd));
        return OkStr(std::move(body));
    }
    if (c == "close") {
        if (!win32::CloseWindow(hwnd)) return ErrStr("post_message_failed");
        json body;
        body["state"] = "closing";
        body["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(hwnd));
        return OkStr(std::move(body));
    }
    const char* target = nullptr;
    if (c == "minimize")  { ok = win32::MinimizeWindow(hwnd); target = "minimized"; }
    else if (c == "maximize") { ok = win32::MaximizeWindow(hwnd); target = "maximized"; }
    else if (c == "restore")  { ok = win32::RestoreWindow(hwnd);  target = "restored"; }
    if (!ok) return ErrStr("show_window_failed");
    json body;
    body["state"] = target;
    body["hwnd"] = static_cast<long long>(reinterpret_cast<intptr_t>(hwnd));
    return OkStr(std::move(body));
}

// ================= 分发 =================

std::string Dispatch(const ParsedArgs& a, std::atomic<bool>* cancel) {
    const std::string& c = a.cmd;
    if (c == "list")            return CmdList(a);
    if (c == "foreground")      return CmdForeground(a);
    if (c == "snapshot")        return CmdSnapshot(a, cancel);
    if (c == "snapshot_all")    return CmdSnapshotAll(a, cancel);
    if (c == "find")            return CmdFind(a, cancel);
    if (c == "click")           return CmdClick(a, cancel);
    if (c == "set_text")        return CmdSetText(a, cancel);
    if (c == "get_text")        return CmdGetText(a, cancel);
    if (c == "scroll")          return CmdScroll(a, cancel);
    if (c == "drag")            return CmdDrag(a);
    if (c == "swipe")           return CmdSwipe(a, cancel);
    if (c == "screenshot")      return CmdScreenshot(a);
    if (c == "minimize" || c == "maximize" || c == "restore" || c == "topmost" || c == "close")
                                 return CmdWindowState(a);
    return ErrStr("unknown_command");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // DPI：进程启动最前调用，Per-Monitor V2，失败退化 SetProcessDPIAware
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        SetProcessDPIAware();
    }
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        fprintf(stderr, "usage: uia_agent <command> [options]   (try --help)\n");
        return 2;
    }
    ParsedArgs args;
    if (!ParseArgs(argc, argv, args)) {
        std::string s = ErrStr("bad_arguments");
        fwrite(s.data(), 1, s.size(), stdout);
        fputc('\n', stdout);
        return 2;
    }
    if (args.cmd.empty() && !args.hasFlag("help")) {
        // 只有 flags 而无命令，除 --help 外一律报错
        std::string s = ErrStr("bad_arguments");
        fwrite(s.data(), 1, s.size(), stdout);
        fputc('\n', stdout);
        return 2;
    }
    if (args.cmd == "help" || args.hasFlag("help")) {
        const char* help =
            "uia_agent - Windows UI Automation agent (single exe, UTF-8 JSON on stdout)\n"
            "usage: uia_agent <command> [options]\n"
            "\n"
            "  list\n"
            "  foreground\n"
            "  snapshot --hwnd <n> [--depth <n>]\n"
            "  snapshot_all [--depth <n>]\n"
            "  find --hwnd <n> --q <json>\n"
            "  click --hwnd <n> --q <json> [--mode semantic|mouse]\n"
            "  click --x <n> --y <n>\n"
            "  set_text --hwnd <n> --q <json> --text <s>\n"
            "  get_text --hwnd <n> --q <json>\n"
            "  scroll --hwnd <n> [--q <json>] [--amount <n>]\n"
            "  drag --x1 <n> --y1 <n> --x2 <n> --y2 <n> [--duration <ms>]\n"
            "  swipe --hwnd <n> [--q <json>] --direction up|down|left|right [--distance <px>]\n"
            "  screenshot --hwnd <n> [--out <file.png>] [--base64]\n"
            "  minimize --hwnd <n>\n"
            "  maximize --hwnd <n>\n"
            "  restore --hwnd <n>\n"
            "  topmost --hwnd <n> [--off]\n"
            "  close --hwnd <n>     (send WM_CLOSE, app may prompt to save)\n"
            "\n"
            "  --help                          show this text\n"
            "  --timeout <ms>                  UIA call timeout (default 5000)\n";
        fputs(help, stdout);
        return 0;
    }

    int timeoutMs = kDefaultTimeoutMs;
    args.getInt("--timeout", timeoutMs);

    // 单 worker 线程执行（内部 CoInitializeEx），主线程限时等待
    std::atomic<bool> cancel{ false };
    auto out = std::make_shared<std::string>();
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    std::thread worker([&] {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
            *out = ErrStr("com_init_failed");
        } else {
            std::string s = Dispatch(args, &cancel);
            if (hr == S_OK) CoUninitialize();
            *out = std::move(s);
        }
        {
            std::lock_guard<std::mutex> lk(m);
            done = true;
        }
        cv.notify_all();
    });

    std::string finalJson;
    {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return done; })) {
            cancel.store(true);
            finalJson = ErrStr("timeout");
            worker.detach(); // 进程即将退出，遗留线程随进程终止
        } else {
            finalJson = *out;
            worker.join();
        }
    }

    fwrite(finalJson.data(), 1, finalJson.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return 0;
}