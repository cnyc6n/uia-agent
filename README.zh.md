# uia_agent — Windows UI Automation 代理（单 exe）

> **[English](./README.md) | 中文**

对 Windows 桌面窗口做：枚举、快照、查找、点击、设值、滚动、拖动、滑动、截屏、窗口状态控制（置顶/最大化/最小化/恢复/关闭）。

- 单文件 exe，C++17 / MSVC / CMake+Ninja 构建，静态 CRT（无 VC++ 运行库依赖，本机 0.44 MB < 5 MB）
- UI 树：Windows UI Automation（`IUIAutomation` / `IUIAutomationElement` / `IUIAutomationTreeWalker`）
- 引用计数：`Microsoft::WRL::ComPtr`（无裸 `Release()`）
- 窗口枚举：`EnumWindows` + `GetWindow(GW_HWNDNEXT)` 取真实 Z-order
- 坐标操作：`SendInput`；截屏：`PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)`
- 图像编码：WIC（系统自带）；JSON：nlohmann 单头文件（本机复用 `D:\文档\C++\json.hpp`，3.12.0）
- 无任何第三方库；全部输出为 UTF-8 JSON，一行一个对象

## 编译

```bat
:: 在 Visual Studio 2022 开发者环境（或先执行 VsDevCmd.bat -arch=amd64）
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
:: 或直接运行仓库内 build.ps1（自动调 vcvars + configure + build）
```

产物：`build\uia_agent.exe`。`CMakeLists.txt` 已设置静态 CRT：

```cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

并带 `/utf-8`、Release `/O2`。依赖系统库：`Uiautomationcore Ole32 User32 OleAut32 Windowscodecs Gdi32 Uuid`（UIAutomationCore / Windowscodecs 为 COM 运行时动态加载，不进静态导入表，属正常）。

## 命令列表

| 命令 | 说明 |
|---|---|
| `list` | 按 Z-order（最前→最后）枚举可见顶层窗口，过滤 `#32768`/`Tooltip`/`SysShadow`/`MSCTFIME UI`/`IME` 类名前缀 |
| `snapshot --hwnd <n> [--depth <n>]` | 单窗口 UIA 控制视图树快照，默认 depth=8，max_nodes=5000 |
| `snapshot_all [--depth <n>]` | 对每个可见顶层窗口快照；双后端：UIA 节点数 ≤ 1 时降级 Win32 枚举；最小化/不可见/拍不到加对应 state |
| `find --hwnd <n> --q <json>` | 按条件查节点。条件：`name`/`automation_id`/`class_name` 子串匹配，`control_type` 全等（均大小写不敏感） |
| `click --hwnd <n> --q <json> [--mode semantic\|mouse]` | 语义：优先 InvokePattern，失败退化 SendInput 移点；`--mode mouse` 强制坐标点击 |
| `click --x <n> --y <n>` | 屏幕物理坐标点击 |
| `set_text --hwnd <n> --q <json> --text <s>` | 优先 ValuePattern::SetValue，失败退化 点击+Ctrl+A+键入（KEYEVENTF_UNICODE，支持中文） |
| `get_text --hwnd <n> --q <json>` | ValuePattern 读值 |
| `scroll --hwnd <n> [--q <json>] [--amount <n>]` | 有 ScrollPattern 走 pattern；否则 rect 中心滚轮（amount>0 下滚，<0 上滚） |
| `drag --x1 <n> --y1 <n> --x2 <n> --y2 <n> [--duration <ms>]` | SendInput 按下→分步移动→抬起，默认 300ms |
| `swipe --hwnd <n> [--q <json>] --direction up\|down\|left\|right [--distance <px>]` | 控件矩形内滑动 |
| `screenshot --hwnd <n> [--out <file.png>] [--base64]` | PrintWindow 抓图，PNG 由 WIC 编码；`--out` 写文件，否则输出 base64 |
| `minimize --hwnd <n>` | SW_MINIMIZE |
| `maximize --hwnd <n>` | SW_MAXIMIZE |
| `restore --hwnd <n>` | SW_RESTORE |
| `topmost --hwnd <n> [--off]` | SetWindowPos 置顶（缺省 on，`--off` 取消） |
| `close --hwnd <n>` | 发送 WM_CLOSE（应用可拦截/提示保存，比强杀友好） |

通用：`--help`；`--timeout <ms>`（UIA 调用超时，默认 5000ms）。

### 示例输出

```json
// list（裸数组，唯一非 ok 包装的输出）
[{"hwnd":527984,"title":"计算器","class_name":"ApplicationFrameWindow","pid":22712},
 {"hwnd":1378628,"title":"test_doc.txt - 记事本","class_name":"Notepad","pid":5964}]

// find
{"count":1,"hwnd":1378628,"nodes":[{"name":"文本编辑器","control_type":"Edit",
  "automation_id":"15","class_name":"Edit",
  "rect":{"left":719,"top":406,"right":1560,"bottom":885,"cx":841,"cy":479},
  "children":[]}],"ok":true,"walked":23}

// set_text / get_text
{"method":"value_pattern","ok":true,"text":"hello"}
{"ok":true,"text":"hello"}

// click（语义 invoke 成功）
{"hwnd":1116574,"method":"invoke_pattern","mode":"semantic","ok":true}

// screenshot --out
{"h":966,"ok":true,"path":"...notepad.png","w":1576}

// 窗口状态（含关闭）
{"hwnd":1116574,"ok":true,"state":"topmost"}
{"hwnd":1116574,"ok":true,"state":"minimized"}
{"hwnd":1116574,"ok":true,"state":"closing"}   // close: 已发送 WM_CLOSE

// 失败
{"ok":false,"err":"not_found"}
{"ok":false,"err":"timeout"}
{"ok":false,"err":"minimized"}
{"ok":false,"err":"invalid_window"}            // close 目标窗口不存在
```

## 关键实现说明

- **DPI**：启动最先 `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`，失败退化 `SetProcessDPIAware()`；所有 rect / 位图 / 鼠标坐标统一物理像素。
- **screenshot 严格路径**：尺寸取 `GetWindowRect` 宽高；`CreateDIBSection` 按 w×h 建 32 位 BGRA、`biHeight=-h`（自上而下）；`PrintWindow` 写 DIB → WIC（`GUID_WICPixelFormat32bppBGRA`）编码 PNG；`PrintWindow` FALSE 或抽样 3×3 网格全 0（全黑/全透明）→ `capture_failed`；`IsIconic` → `minimized`；w/h≤0 → `bad_window_rect`。GDI 对象（HDC/HBITMAP）经 RAII 成对释放。
- **超时**：UIA 遍历在独立 worker 线程（内部 `CoInitializeEx`），主线程条件变量定时等待，超时置 cancel 并输出 `{"ok":false,"err":"timeout"}`，进程随即退出。
- **BoundingRectangle 语义自适应**：实测 Win32 控件返回 `[left,top,right,bottom]`，而 UWP/XAML provider（如计算器）返回 `[left,top,width,height]`。解码时若 `right<left || bottom<top` 视为宽高并重算，保证两种后端坐标正确（见 `uia.cpp::PropRect`）。
- **列表过滤**：类名以 `#32768`、`Tooltip`、`SysShadow`、`MSCTFIME UI`、`IME` 开头的窗口不进 list / snapshot_all。

## 已知限制

- 自绘 UI、游戏、DirectX、默认 Qt 拍不到 UIA 树：`snapshot` 返回 `{"ok":false,"err":"unsupported"}`，`snapshot_all` 该项 `state` 含 `unsupported`；可改用 screenshot + 上层图像识别兜底。
- 管理员窗口需本 exe 以管理员运行，否则 UIPI 拦截（SetForegroundWindow / SendInput 大多仍可用，InvokePattern 可能被拒）。
- Chromium/Electron 默认关闭 accessibility 时 UIA 为空（已在 Edge 上验证：开启后能取到外壳 4 节点）；VS Code 未装，未实测。
- UIA 调用可能挂起：已加 5000ms 超时（`--timeout` 可调）；超时线程随进程退出终止，不保证被挂起的 COM 调用完成。
- PrintWindow 对个别 DRM / 自绘（UWP 部分窗口）窗口可能返回黑屏 → `capture_failed`，属系统限制。
- 多显示器时 SendInput 绝对坐标以主屏为准（`MOUSEEVENTF_ABSOLUTE` 无 `VIRTUALDESK`）；副屏负坐标不支持。
- 计算器（UWP）等可访问性树语言随系统语言（中文系统按钮名"七"而非"7"），示例查询多用 `automation_id`。

## 自测结果（本机 Windows 10 x64，VS2022，calc/notepad 实录）

| # | 用例 | 结果 |
|---|---|---|
| 1 | `list` Z-order 枚举（记事本、计算器、Edge…22-23 个可见窗口，顺序与 Z-order 一致） | ✅ |
| 2 | `snapshot --hwnd <记事本>`（depth=4，23 节点：菜单/编辑框/状态栏/滚动条齐全） | ✅ |
| 3 | `find --q {"control_type":"Edit"}` 命中编辑框，rect 正确 | ✅ |
| 4 | `set_text --q {"control_type":"Edit"} --text hello`（value_pattern） | ✅ |
| 5 | `get_text` 读回 "hello" | ✅ |
| 6 | 计算器 `click --q {"automation_id":"num7Button"}` (invoke_pattern)，显示器变"显示为 7"、再点成 77 | ✅ |
| 7 | `click --x 1222 --y 1035` 坐标点击（显示从 77 变 7,755，证明命中） | ✅ |
| 8 | `screenshot --out notepad.png` → PNG 1576×966 严格等于 GetWindowRect 宽高，图像查看/视觉识别标题栏+菜单+"hello"正确 | ✅ |
| 9 | `screenshot --base64` → 还原后 PNG 签名 `89 50 4E 47...`、尺寸一致 | ✅ |
| 10 | Edge（Chromium 内核）`snapshot` 取到外壳（Window + 4 节点），snapshot_all 无 unsupported | ✅ |
| 附加 | `scroll`/`drag`/`swipe`、`minimize`/`maximize`/`restore`/`topmost(--off)` 全部实测生效（IsIconic/IsZoomed/WS_EX_TOPMOST 断言） | ✅ |
| 附加 | 超时兜底 `--timeout 10` → `{"err":"timeout"}`；最小化截图 → `{"err":"minimized"}` | ✅ |

构建回归脚本 `build.ps1`、测试脚本 `tests\run_tests.ps1` 已在仓库内。
