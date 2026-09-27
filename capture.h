#pragma once
// 截屏：PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT) + WIC PNG 编码。
#include <string>
#include <windows.h>

namespace cap {

struct Shot {
    bool ok = false;
    std::string err;   // "minimized" / "bad_window_rect" / "capture_failed" / ...
    int w = 0, h = 0;  // 物理像素
    std::string png;   // PNG 字节（WIC 编码）
};

Shot CaptureWindow(HWND hwnd);

std::string Base64Encode(const void* data, size_t len);

} // namespace cap