# uia_agent — Windows UI Automation agent (single exe)

> **[中文](./README.zh.md) | English**

Enumerate, snapshot, find, click, set text, scroll, drag, swipe, screenshot, and
control window state (topmost / maximize / minimize / restore / close) on Windows
desktop windows.

- Single-file exe, C++17 / MSVC / CMake+Ninja, static CRT (no VC++ runtime
  dependency; 0.44 MB on this machine, under the 5 MB target)
- UI tree: Windows UI Automation (`IUIAutomation` / `IUIAutomationElement` /
  `IUIAutomationTreeWalker`)
- Reference counting: `Microsoft::WRL::ComPtr` (no raw `Release()`)
- Window enumeration: `EnumWindows` + `GetWindow(GW_HWNDNEXT)` for real Z-order
- Coordinate ops: `SendInput`; screenshot: `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT)`
- Image encoding: WIC (system-provided); JSON: nlohmann single header
  (this machine reuses `D:\文档\C++\json.hpp`, 3.12.0)
- No third-party libraries; all output is UTF-8 JSON, one object per line

## Build

```bat
:: In a Visual Studio 2022 developer shell (or run VsDevCmd.bat -arch=amd64 first)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
:: or simply run build.ps1 in the repo root (vcvars + configure + build)
```

Artifact: `build\uia_agent.exe`. `CMakeLists.txt` sets the static CRT:

```cmake
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

plus `/utf-8` and Release `/O2`. Linked system libraries:
`Uiautomationcore Ole32 User32 OleAut32 Windowscodecs Gdi32 Uuid`
(UIAutomationCore / Windowscodecs are loaded dynamically by COM at runtime, so
they are not in the static import table — that is expected).

## Commands

| Command | Description |
|---|---|
| `list` | Enumerate visible top-level windows by Z-order (front → back); filters out `#32768` / `Tooltip` / `SysShadow` / `MSCTFIME UI` / `IME` class-name prefixes |
| `snapshot --hwnd <n> [--depth <n>]` | UIA control-tree snapshot of one window; default depth=8, max_nodes=5000 |
| `snapshot_all [--depth <n>]` | Snapshot every visible top-level window; dual backend: falls back to Win32 enumeration when UIA yields ≤1 node; adds `minimized` / `invisible` / `unsupported` states |
| `find --hwnd <n> --q <json>` | Find nodes by condition. `name` / `automation_id` / `class_name` substring (case-insensitive), `control_type` exact (case-insensitive) |
| `click --hwnd <n> --q <json> [--mode semantic\|mouse]` | Semantic: InvokePattern first, falls back to SendInput move+click; `--mode mouse` forces coordinate click |
| `click --x <n> --y <n>` | Screen-physical-coordinate click |
| `set_text --hwnd <n> --q <json> --text <s>` | ValuePattern::SetValue first, falls back to click + Ctrl+A + type (KEYEVENTF_UNICODE, CJK-safe) |
| `get_text --hwnd <n> --q <json>` | Read value via ValuePattern |
| `scroll --hwnd <n> [--q <json>] [--amount <n>]` | ScrollPattern if present; otherwise wheel at rect center (amount>0 = down, <0 = up) |
| `drag --x1 <n> --y1 <n> --x2 <n> --y2 <n> [--duration <ms>]` | SendInput press → stepped move → release; default 300ms |
| `swipe --hwnd <n> [--q <json>] --direction up\|down\|left\|right [--distance <px>]` | Swipe inside the control rect |
| `screenshot --hwnd <n> [--out <file.png>] [--base64]` | PrintWindow capture, PNG via WIC; `--out` writes a file, otherwise base64 |
| `minimize --hwnd <n>` | SW_MINIMIZE |
| `maximize --hwnd <n>` | SW_MAXIMIZE |
| `restore --hwnd <n>` | SW_RESTORE |
| `topmost --hwnd <n> [--off]` | SetWindowPos always-on-top (default on; `--off` cancels) |
| `close --hwnd <n>` | Send WM_CLOSE (app may intercept / prompt to save; friendlier than force-kill) |

Common: `--help`; `--timeout <ms>` (UIA call timeout, default 5000ms).

### Example output

```json
// list (bare array — the only output without an ok wrapper)
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

// click (semantic invoke)
{"hwnd":1116574,"method":"invoke_pattern","mode":"semantic","ok":true}

// screenshot --out
{"h":966,"ok":true,"path":"...notepad.png","w":1576}

// window state (incl. close)
{"hwnd":1116574,"ok":true,"state":"topmost"}
{"hwnd":1116574,"ok":true,"state":"minimized"}
{"hwnd":1116574,"ok":true,"state":"closing"}   // close: WM_CLOSE sent

// failures
{"ok":false,"err":"not_found"}
{"ok":false,"err":"timeout"}
{"ok":false,"err":"minimized"}
{"ok":false,"err":"invalid_window"}            // close target window missing
```

## Key implementation notes

- **DPI**: calls `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`
  first, falls back to `SetProcessDPIAware()`; all rects / bitmaps / mouse
  coordinates are physical pixels.
- **screenshot strict path**: size from `GetWindowRect`; `CreateDIBSection` with
  w×h 32-bit BGRA, `biHeight=-h` (top-down); `PrintWindow` into the DIB → WIC
  (`GUID_WICPixelFormat32bppBGRA`) → PNG; `PrintWindow` FALSE or all-zero 3×3
  sample grid (black / fully transparent) → `capture_failed`; `IsIconic` →
  `minimized`; w/h≤0 → `bad_window_rect`. GDI objects (HDC / HBITMAP) are freed
  in pairs via RAII.
- **Timeout**: UIA traversal runs on a worker thread (internal `CoInitializeEx`);
  the main thread waits on a condition variable and sets cancel + emits
  `{"ok":false,"err":"timeout"}` on expiry; the process then exits.
- **BoundingRectangle adaptive decode**: Win32 controls return
  `[left,top,right,bottom]` while UWP/XAML providers (e.g. Calculator) return
  `[left,top,width,height]`. When `right<left || bottom<top`, the last two
  values are treated as width/height and recomputed (see `uia.cpp::PropRect`).
- **list filtering**: windows whose class name starts with `#32768`, `Tooltip`,
  `SysShadow`, `MSCTFIME UI`, or `IME` are excluded from list / snapshot_all.

## Known limits

- Self-drawn UI, games, DirectX, stock Qt expose no UIA tree: `snapshot` returns
  `{"ok":false,"err":"unsupported"}`, `snapshot_all` marks the entry `unsupported`;
  fall back to screenshot + image recognition.
- Elevated windows require the exe to run as administrator, else UIPI blocks
  (SetForegroundWindow / SendInput usually still work; InvokePattern may be denied).
- Chromium/Electron disable accessibility by default, so UIA may be empty on
  first access (verified on Edge: after enabling, the shell is reachable with
  4 nodes; VS Code not installed here, untested).
- UIA calls may hang: a 5000ms timeout is built in (`--timeout` adjustable);
  the timed-out thread terminates with the process — a hung COM call is not
  guaranteed to finish.
- PrintWindow may return a black frame for some DRM / self-drawn (some UWP)
  windows → `capture_failed`, a system limitation.
- On multi-monitor setups SendInput absolute coordinates use the primary
  display (`MOUSEEVENTF_ABSOLUTE` without `VIRTUALDESK`); negative coords on
  secondary displays are unsupported.
- Accessibility tree language follows the system language (Chinese system:
  Calculator button is named "七" not "7"); prefer `automation_id` in queries.

## Self-test results (this machine: Windows 10 x64, VS2022, calc/notepad)

| # | Case | Result |
|---|---|---|
| 1 | `list` Z-order enumeration (notepad / calc / Edge … 22-23 visible windows, order matches Z-order) | ✅ |
| 2 | `snapshot --hwnd <notepad>` (depth=4, 23 nodes: menu / edit / status bar / scrollbar) | ✅ |
| 3 | `find --q {"control_type":"Edit"}` hits the edit box, rect correct | ✅ |
| 4 | `set_text --q {"control_type":"Edit"} --text hello` (value_pattern) | ✅ |
| 5 | `get_text` reads back "hello" | ✅ |
| 6 | Calculator `click --q {"automation_id":"num7Button"}` (invoke_pattern), display shows "显示为 7", then 77 | ✅ |
| 7 | `click --x 1222 --y 1035` coordinate click (display 77 → 7,755, proves hit) | ✅ |
| 8 | `screenshot --out notepad.png` → PNG 1576×966 exactly equals GetWindowRect; title bar + menu + "hello" verified visually | ✅ |
| 9 | `screenshot --base64` → decoded PNG signature `89 50 4E 47...`, size matches | ✅ |
| 10 | Edge (Chromium core) `snapshot` reaches the shell (Window + 4 nodes), snapshot_all has no unsupported | ✅ |
| extra | `scroll` / `drag` / `swipe`, `minimize` / `maximize` / `restore` / `topmost(--off)` all verified (IsIconic / IsZoomed / WS_EX_TOPMOST assertions) | ✅ |
| extra | timeout fallback `--timeout 10` → `{"err":"timeout"}`; minimized screenshot → `{"err":"minimized"}` | ✅ |

Build script `build.ps1` and test script `tests\run_tests.ps1` are in the repo.
