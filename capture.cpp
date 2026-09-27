#include "capture.h"

#define INITGUID  // 在本 TU 定义 WIC GUID 符号（wincodec.h 的 DEFINE_GUID）
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace cap {

namespace {

struct GdiGuard {
    HGDIOBJ obj = nullptr;
    ~GdiGuard() { if (obj) DeleteObject(obj); }
};

struct DcGuard {
    HDC dc = nullptr;
    ~DcGuard() { if (dc) DeleteDC(dc); }
};

// 抽样 3x3 网格：全为 0（黑/透明）则视为 capture 失败
bool HasVisiblePixels(const void* bits, int w, int h) {
    if (w <= 0 || h <= 0 || !bits) return false;
    const BYTE* p = static_cast<const BYTE*>(bits);
    const int stride = w * 4;
    const int xs[3] = { 0, w / 2, w - 1 };
    const int ys[3] = { 0, h / 2, h - 1 };
    for (int yy : ys) {
        for (int xx : xs) {
            if (xx < 0 || xx >= w || yy < 0 || yy >= h) continue;
            const BYTE* px = p + static_cast<__int64>(yy) * stride + xx * 4;
            if (px[0] | px[1] | px[2] | px[3]) return true;
        }
    }
    return false;
}

// WIC: BGRA 行数据 -> PNG 字节（内存流）
bool EncodePng(const void* bits, int w, int h, std::string& out) {
    HRESULT hr;
    bool needUninit = false;
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) {
        needUninit = true;             // 本次初始化成功，需配对 Uninitialize
    } else if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return false;                  // RPC_E_CHANGED_MODE: 已有 STA，也能用 WIC
    }

    bool ok = false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> memStream;   // 内存 IStream
    ComPtr<IWICStream> wicStream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;

    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) || !factory) break;
        // CreateStreamOnHGlobal -> IStream 内存流（TRUE = 自动释放缓冲）
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &memStream)) || !memStream) break;
        if (FAILED(factory->CreateStream(&wicStream)) || !wicStream) break;
        if (FAILED(wicStream->InitializeFromIStream(memStream.Get()))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || !encoder) break;
        if (FAILED(encoder->Initialize(wicStream.Get(), WICBitmapEncoderNoCache))) break;
        IPropertyBag2* props = nullptr;
        if (FAILED(encoder->CreateNewFrame(&frame, &props)) || !frame) break;
        frame->Initialize(props);
        if (props) props->Release();
        if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt))) break;
        UINT stride = static_cast<UINT>(w) * 4;
        if (FAILED(frame->WritePixels(static_cast<UINT>(h), stride,
                                      static_cast<UINT>(h) * stride,
                                      static_cast<BYTE*>(const_cast<void*>(bits))))) break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;

        // 取回内存流字节
        STATSTG stat = {};
        if (FAILED(memStream->Stat(&stat, STATFLAG_NONAME))) break;
        ULONGLONG sz = stat.cbSize.QuadPart;
        HGLOBAL hg = nullptr;
        if (sz == 0 || FAILED(GetHGlobalFromStream(memStream.Get(), &hg)) || !hg) break;
        void* p = GlobalLock(hg);
        if (!p) break;
        out.assign(static_cast<const char*>(p), static_cast<size_t>(sz));
        GlobalUnlock(hg);
        ok = !out.empty();
    } while (false);

    if (needUninit) CoUninitialize();
    return ok;
}

} // namespace

Shot CaptureWindow(HWND hwnd) {
    Shot s;
    if (!hwnd || !IsWindow(hwnd)) { s.err = "invalid_window"; return s; }
    if (IsIconic(hwnd)) { s.err = "minimized"; return s; }

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) { s.err = "invalid_window"; return s; }
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    if (w <= 0 || h <= 0) { s.err = "bad_window_rect"; return s; }
    s.w = w;
    s.h = h;

    // CreateDIBSection：32 位 BGRA，biHeight=-h 自上而下，尺寸天然 = wr 宽高
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // 顶-下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hbmp = CreateDIBSection(screenDc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);
    if (!hbmp || !bits) {
        if (hbmp) DeleteObject(hbmp);
        s.err = "dib_failed";
        return s;
    }
    GdiGuard bmpGuard{hbmp};

    DcGuard memDc;
    memDc.dc = CreateCompatibleDC(nullptr);
    if (!memDc.dc) { s.err = "dc_failed"; return s; }
    HGDIOBJ old = SelectObject(memDc.dc, hbmp);

    BOOL ok = PrintWindow(hwnd, memDc.dc, PW_RENDERFULLCONTENT);
    SelectObject(memDc.dc, old);

    if (!ok || !HasVisiblePixels(bits, w, h)) {
        s.err = "capture_failed";
        return s;
    }
    if (!EncodePng(bits, w, h, s.png)) {
        s.err = "encode_failed";
        return s;
    }
    s.ok = true;
    return s;
}

std::string Base64Encode(const void* data, size_t len) {
    static const char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    const BYTE* p = static_cast<const BYTE*>(data);
    for (size_t i = 0; i < len; i += 3) {
        unsigned a = p[i];
        unsigned b = (i + 1 < len) ? p[i + 1] : 0;
        unsigned c = (i + 2 < len) ? p[i + 2] : 0;
        out.push_back(kAlpha[(a >> 2) & 0x3F]);
        out.push_back(kAlpha[((a << 4) | (b >> 4)) & 0x3F]);
        out.push_back((i + 1 < len) ? kAlpha[((b << 2) | (c >> 6)) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kAlpha[c & 0x3F] : '=');
    }
    return out;
}

} // namespace cap