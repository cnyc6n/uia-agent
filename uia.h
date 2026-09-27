#pragma once
// UIA 封装：初始化、树遍历、查找、Pattern 操作。
// 线程约束：所有 UIA 调用必须跑在同一个已 CoInitializeEx 的线程上，
// 上层（main.cpp）负责在 worker 线程内初始化/清理 COM。
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

struct UiRect {
    long left = 0, top = 0, right = 0, bottom = 0;
    long cx() const { return right - left; }
    long cy() const { return bottom - top; }
};

struct UiNode {
    std::string name;         // UTF-8
    std::string control_type; // 如 "Button" / "Edit"
    std::string automation_id;
    std::string class_name;
    UiRect rect;              // 物理像素（屏幕坐标）
    std::vector<UiNode> children;
};

struct QueryFilter {
    bool has_name = false;
    bool has_aid = false;
    bool has_type = false;
    bool has_class = false;
    std::string name;         // 子串匹配（大小写不敏感）
    std::string automation_id; // 子串匹配（大小写不敏感）
    std::string control_type;  // 全等匹配（大小写不敏感）
    std::string class_name;    // 子串匹配（大小写不敏感）
};

// 不透明元素引用，由 UiaFindElements 产生，仅在同一命令线程内有效。
using UiaRef = std::shared_ptr<void>;
struct UiaHit {
    UiaRef ref;      // 存活期内的 IUIAutomationElement*
    UiNode info;     // 命中节点的快照
};

// 构建窗口的 UIA 控制视图树。返回节点总数（含根）；0 表示完全不可用。
int UiaBuildTree(HWND hwnd, int maxDepth, int maxNodes,
                 std::atomic<bool>* cancel, UiNode& root);

// 在窗口树中按过滤器查找，返回遍历到的节点总数；命中收集进 hits。
int UiaFindElements(HWND hwnd, const QueryFilter& f, int maxNodes,
                    std::atomic<bool>* cancel, std::vector<UiaHit>& hits);

// Pattern 操作：全部返回 bool/int，绝不抛异常。
bool UiaInvoke(const UiaHit& hit);                       // IUIAutomationInvokePattern
bool UiaValuePatternGet(const UiaHit& hit, std::string& out); // 成功返回 true，out 为文本
bool UiaValuePatternSet(const UiaHit& hit, const std::string& text);
int  UiaScrollViaPattern(const UiaHit& hit, int amount); // 1=已滚, 0=无 pattern/不支持(落滚轮)