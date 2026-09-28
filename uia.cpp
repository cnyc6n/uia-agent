#include "uia.h"
#include <windows.h>
#include <unknwn.h>    // MIDL_INTERFACE / IUnknown（WIN32_LEAN_AND_MEAN 抑制了 windows.h 的自动引入）
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <oleauto.h>   // SysStringLen / SafeArrayGetElement
#include <algorithm>
#include "util.h"

using Microsoft::WRL::ComPtr;

namespace {

// ---- 字符串属性 ----
std::string PropStr(IUIAutomationElement* el, PROPERTYID pid) {
    VARIANT v;
    VariantInit(&v);
    std::string out;
    if (el && SUCCEEDED(el->GetCurrentPropertyValue(pid, &v)) && v.vt == VT_BSTR && v.bstrVal) {
        UINT len = SysStringLen(v.bstrVal);
        out = WideToUtf8(v.bstrVal, static_cast<int>(len));
    }
    VariantClear(&v);
    return out;
}

long PropControlType(IUIAutomationElement* el) {
    VARIANT v;
    VariantInit(&v);
    long id = 0;
    if (el && SUCCEEDED(el->GetCurrentPropertyValue(UIA_ControlTypePropertyId, &v))) {
        if (v.vt == VT_I4) id = v.lVal;
        else if (v.vt == VT_I8) id = static_cast<long>(v.llVal);
        else if (v.vt == VT_UI4) id = static_cast<long>(v.ulVal);
    }
    VariantClear(&v);
    return id;
}

// ControlType id -> 字符串名（ControlViewTree 常用类型；未知归为 "Custom"）
const char* ControlTypeName(long id) {
    switch (id) {
        case UIA_ButtonControlTypeId:       return "Button";
        case UIA_CalendarControlTypeId:     return "Calendar";
        case UIA_CheckBoxControlTypeId:     return "CheckBox";
        case UIA_ComboBoxControlTypeId:     return "ComboBox";
        case UIA_EditControlTypeId:         return "Edit";
        case UIA_HyperlinkControlTypeId:    return "Hyperlink";
        case UIA_ImageControlTypeId:        return "Image";
        case UIA_ListItemControlTypeId:     return "ListItem";
        case UIA_ListControlTypeId:         return "List";
        case UIA_MenuControlTypeId:         return "Menu";
        case UIA_MenuBarControlTypeId:      return "MenuBar";
        case UIA_MenuItemControlTypeId:     return "MenuItem";
        case UIA_ProgressBarControlTypeId:  return "ProgressBar";
        case UIA_RadioButtonControlTypeId:  return "RadioButton";
        case UIA_ScrollBarControlTypeId:    return "ScrollBar";
        case UIA_SliderControlTypeId:       return "Slider";
        case UIA_SpinnerControlTypeId:      return "Spinner";
        case UIA_StatusBarControlTypeId:    return "StatusBar";
        case UIA_TabControlTypeId:          return "Tab";
        case UIA_TabItemControlTypeId:      return "TabItem";
        case UIA_TextControlTypeId:         return "Text";
        case UIA_ToolBarControlTypeId:      return "ToolBar";
        case UIA_ToolTipControlTypeId:      return "ToolTip";
        case UIA_TreeControlTypeId:         return "Tree";
        case UIA_TreeItemControlTypeId:     return "TreeItem";
        case UIA_CustomControlTypeId:       return "Custom";
        case UIA_GroupControlTypeId:        return "Group";
        case UIA_ThumbControlTypeId:        return "Thumb";
        case UIA_DataGridControlTypeId:     return "DataGrid";
        case UIA_DataItemControlTypeId:     return "DataItem";
        case UIA_DocumentControlTypeId:     return "Document";
        case UIA_SplitButtonControlTypeId:  return "SplitButton";
        case UIA_WindowControlTypeId:       return "Window";
        case UIA_PaneControlTypeId:         return "Pane";
        case UIA_HeaderControlTypeId:       return "Header";
        case UIA_HeaderItemControlTypeId:   return "HeaderItem";
        case UIA_TableControlTypeId:        return "Table";
        case UIA_TitleBarControlTypeId:     return "TitleBar";
        case UIA_SeparatorControlTypeId:    return "Separator";
        default:                            return "Custom";
    }
}

UiRect PropRect(IUIAutomationElement* el) {
    UiRect r;
    if (!el) return r;
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_BoundingRectanglePropertyId, &v))) {
        // BoundingRectangle 为 4 元 SAFEARRAY；元素类型因 provider 而异
        // （大部分为 double VT_R8；个别 XAML provider 用 float VT_R4）
        if ((v.vt & VT_ARRAY) && v.parray) {
            SAFEARRAY* sa = v.parray;
            VARTYPE elemType = VT_EMPTY;
            if (SUCCEEDED(SafeArrayGetVartype(sa, &elemType))) {
                long lb = 0, ub = -1;
                if (SafeArrayGetDim(sa) == 1 &&
                    SUCCEEDED(SafeArrayGetLBound(sa, 1, &lb)) &&
                    SUCCEEDED(SafeArrayGetUBound(sa, 1, &ub))) {
                    long n = ub - lb + 1;
                    void* data = nullptr;
                    if (n >= 4 && SUCCEEDED(SafeArrayAccessData(sa, &data))) {
                        double vals[4] = {0, 0, 0, 0};
                        if (elemType == VT_R8) {
                            const double* d = static_cast<const double*>(data);
                            for (int i = 0; i < 4; ++i) vals[i] = d[i];
                        } else if (elemType == VT_R4) {
                            const float* f = static_cast<const float*>(data);
                            for (int i = 0; i < 4; ++i) vals[i] = f[i];
                        }
                        SafeArrayUnaccessData(sa);

                        // 语义自适应：Win32 控件给 [left,top,right,bottom]，
                        // 部分 XAML/UWP provider 给 [left,top,width,height]。
                        // 合法 rect 必有 right>=left 且 bottom>=top；若违反则视为宽高。
                        if (vals[2] >= vals[0] && vals[3] >= vals[1]) {
                            r.left = static_cast<long>(vals[0] + 0.5);
                            r.top = static_cast<long>(vals[1] + 0.5);
                            r.right = static_cast<long>(vals[2] + 0.5);
                            r.bottom = static_cast<long>(vals[3] + 0.5);
                        } else {
                            r.left = static_cast<long>(vals[0] + 0.5);
                            r.top = static_cast<long>(vals[1] + 0.5);
                            r.right = static_cast<long>(vals[0] + vals[2] + 0.5);
                            r.bottom = static_cast<long>(vals[1] + vals[3] + 0.5);
                        }
                    }
                }
            }
        }
    }
    VariantClear(&v);
    // 防御：隐藏/屏幕外元素可能返回 right<=left 或 bottom<=top 的非法矩形，
    // 统一归零，避免下游 cx/cy 为负导致坐标错位。
    if (r.right <= r.left || r.bottom <= r.top) {
        r.left = r.top = r.right = r.bottom = 0;
    }
    return r;
}

void FillNode(IUIAutomationElement* el, UiNode& n) {
    n.name = PropStr(el, UIA_NamePropertyId);
    n.control_type = ControlTypeName(PropControlType(el));
    n.automation_id = PropStr(el, UIA_AutomationIdPropertyId);
    n.class_name = PropStr(el, UIA_ClassNamePropertyId);
    n.rect = PropRect(el);
}

// 控制视图遍历器（只取一次，避免每层重复查询）
ComPtr<IUIAutomationTreeWalker> GetWalker(ComPtr<IUIAutomation>& au) {
    ComPtr<IUIAutomationTreeWalker> walker;
    au->get_ControlViewWalker(&walker);
    return walker;
}

constexpr long kFindMaxDepth = 64; // find 不受层级深限制，靠 maxNodes 兜底

// 大小写不敏感子串（ASCII 折叠；非 ASCII 逐字节比较）
bool ContainsCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    std::string h = hay, n = needle;
    for (auto& c : h) if (c >= 'A' && c <= 'Z') c += 32;
    for (auto& c : n) if (c >= 'A' && c <= 'Z') c += 32;
    return h.find(n) != std::string::npos;
}

bool Matches(const UiNode& n, const QueryFilter& f) {
    if (f.has_name && !ContainsCI(n.name, f.name)) return false;
    if (f.has_aid && !ContainsCI(n.automation_id, f.automation_id)) return false;
    if (f.has_type && !ContainsCI(n.control_type, f.control_type)) return false;
    if (f.has_class && !ContainsCI(n.class_name, f.class_name)) return false;
    return true;
}

// 元素引用包装：shared_ptr<void> 指向堆上的 ComPtr；交给调用者持有，同线程内有效
UiaRef WrapElement(ComPtr<IUIAutomationElement> el) {
    auto* holder = new ComPtr<IUIAutomationElement>(std::move(el));
    return UiaRef(holder, [](void* p) {
        delete static_cast<ComPtr<IUIAutomationElement>*>(p);
    });
}

ComPtr<IUIAutomationElement>& Unwrap(const UiaRef& r) {
    return *static_cast<ComPtr<IUIAutomationElement>*>(r.get());
}

// 建一个可用的 CUIAutomation + 根元素
bool GetRoot(HWND hwnd, ComPtr<IUIAutomation>& au, ComPtr<IUIAutomationElement>& root) {
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&au)))) return false;
    if (FAILED(au->ElementFromHandle(hwnd, &root))) return false;
    return root != nullptr;
}

} // namespace

int UiaBuildTree(HWND hwnd, int maxDepth, int maxNodes,
                 std::atomic<bool>* cancel, UiNode& root) {
    ComPtr<IUIAutomation> au;
    ComPtr<IUIAutomationElement> el;
    if (!GetRoot(hwnd, au, el)) return 0;
    auto walker = GetWalker(au);
    int count = 0;

    // 递归构建。maxNodes 在做工量上首先封顶，避免爆内存。
    struct Builder {
        ComPtr<IUIAutomation>& au;
        ComPtr<IUIAutomationTreeWalker>& walker;
        int maxDepth;
        int maxNodes;
        std::atomic<bool>* cancel;
        int& count;
        void Run(ComPtr<IUIAutomationElement>& cur, int depth, UiNode& out) {
            if (cancel && cancel->load()) return;
            if (count >= maxNodes) return;
            ++count;
            FillNode(cur.Get(), out);
            if (depth >= maxDepth) return;
            ComPtr<IUIAutomationElement> child;
            HRESULT hr = walker->GetFirstChildElement(cur.Get(), &child);
            while (SUCCEEDED(hr) && child) {
                if (cancel && cancel->load()) break;
                if (count >= maxNodes) break;
                UiNode c;
                Run(child, depth + 1, c);
                out.children.push_back(std::move(c));
                ComPtr<IUIAutomationElement> next;
                hr = walker->GetNextSiblingElement(child.Get(), &next);
                child = next;
            }
        }
    } b{au, walker, maxDepth, maxNodes, cancel, count};

    b.Run(el, 0, root);
    return count;
}

int UiaFindElements(HWND hwnd, const QueryFilter& f, int maxNodes,
                    std::atomic<bool>* cancel, std::vector<UiaHit>& hits) {
    ComPtr<IUIAutomation> au;
    ComPtr<IUIAutomationElement> el;
    if (!GetRoot(hwnd, au, el)) return 0;
    auto walker = GetWalker(au);
    int count = 0;

    struct Finder {
        ComPtr<IUIAutomation>& au;
        ComPtr<IUIAutomationTreeWalker>& walker;
        int maxNodes;
        std::atomic<bool>* cancel;
        int& count;
        const QueryFilter& f;
        std::vector<UiaHit>& hits;
        void Run(ComPtr<IUIAutomationElement>& cur, int depth) {
            if (cancel && cancel->load()) return;
            if (count >= maxNodes) return;
            ++count;
            UiNode n;
            FillNode(cur.Get(), n);
            if (Matches(n, f)) {
                UiaHit h;
                h.info = std::move(n);
                h.ref = WrapElement(cur); // ComPtr 拷贝（AddRef）
                hits.push_back(std::move(h));
            }
            if (depth >= kFindMaxDepth) return;
            ComPtr<IUIAutomationElement> child;
            HRESULT hr = walker->GetFirstChildElement(cur.Get(), &child);
            while (SUCCEEDED(hr) && child) {
                if (cancel && cancel->load()) break;
                if (count >= maxNodes) break;
                Run(child, depth + 1);
                ComPtr<IUIAutomationElement> next;
                hr = walker->GetNextSiblingElement(child.Get(), &next);
                child = next;
            }
        }
    } fdr{au, walker, maxNodes, cancel, count, f, hits};

    fdr.Run(el, 0);
    return count;
}

bool UiaInvoke(const UiaHit& hit) {
    if (!hit.ref) return false;
    auto& el = Unwrap(hit.ref);
    ComPtr<IUnknown> unk;
    if (FAILED(el->GetCurrentPattern(UIA_InvokePatternId, unk.GetAddressOf())) || !unk) return false;
    ComPtr<IUIAutomationInvokePattern> ip;
    if (FAILED(unk.As(&ip))) return false;
    return SUCCEEDED(ip->Invoke());
}

bool UiaGetProps(const UiaHit& hit, UiProps& out) {
    out = UiProps{};
    if (!hit.ref) return false;
    auto& el = Unwrap(hit.ref);
    if (!el) return false;
    bool any = false;

    VARIANT v; VariantInit(&v);
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_IsEnabledPropertyId, &v))) {
        if (v.vt == VT_BOOL) { out.enabled = v.boolVal != 0; any = true; }
    }
    VariantClear(&v); VariantInit(&v);
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_IsOffscreenPropertyId, &v))) {
        if (v.vt == VT_BOOL) { out.offscreen = v.boolVal != 0; any = true; }
    }
    VariantClear(&v); VariantInit(&v);
    if (SUCCEEDED(el->GetCurrentPropertyValue(UIA_IsKeyboardFocusablePropertyId, &v))) {
        if (v.vt == VT_BOOL) { out.focusable = v.boolVal != 0; any = true; }
    }
    VariantClear(&v);

    // ValuePattern 文本
    ComPtr<IUnknown> unk;
    if (SUCCEEDED(el->GetCurrentPattern(UIA_ValuePatternId, unk.GetAddressOf())) && unk) {
        ComPtr<IUIAutomationValuePattern> vp;
        if (SUCCEEDED(unk.As(&vp))) {
            BSTR val = nullptr;
            if (SUCCEEDED(vp->get_CurrentValue(&val)) && val) {
                out.hasValue = true;
                out.value = WideToUtf8(val, static_cast<int>(SysStringLen(val)));
                SysFreeString(val);
                any = true;
            }
        }
    }
    return any;
}

bool UiaValuePatternGet(const UiaHit& hit, std::string& out) {
    if (!hit.ref) return false;
    auto& el = Unwrap(hit.ref);
    ComPtr<IUnknown> unk;
    if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, unk.GetAddressOf())) || !unk) return false;
    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(unk.As(&vp))) return false;
    BSTR b = nullptr;
    if (FAILED(vp->get_CurrentValue(&b))) return false;
    if (b) {
        out = WideToUtf8(b, static_cast<int>(SysStringLen(b)));
        SysFreeString(b);
    } else {
        out.clear();
    }
    return true;
}

bool UiaValuePatternSet(const UiaHit& hit, const std::string& text) {
    if (!hit.ref) return false;
    auto& el = Unwrap(hit.ref);
    ComPtr<IUnknown> unk;
    if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, unk.GetAddressOf())) || !unk) return false;
    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(unk.As(&vp))) return false;
    std::wstring w = Utf8ToWide(text);
    BSTR b = SysAllocStringLen(w.c_str(), static_cast<UINT>(w.size()));
    if (!b) return false;
    HRESULT hr = vp->SetValue(b);
    SysFreeString(b);
    return SUCCEEDED(hr);
}

int UiaScrollViaPattern(const UiaHit& hit, int amount) {
    if (!hit.ref) return 0;
    auto& el = Unwrap(hit.ref);
    ComPtr<IUnknown> unk;
    if (FAILED(el->GetCurrentPattern(UIA_ScrollPatternId, unk.GetAddressOf())) || !unk) return 0;
    ComPtr<IUIAutomationScrollPattern> sp;
    if (FAILED(unk.As(&sp))) return 0;
    if (amount < 0) return 0; // pattern 无"减少"语义，交给滚轮
    ScrollAmount vam = (llabs(amount) >= 2) ? ScrollAmount_LargeIncrement
                                            : ScrollAmount_SmallIncrement;
    return SUCCEEDED(sp->Scroll(ScrollAmount_NoAmount, vam)) ? 1 : 0;
}