#include "image_win.hpp"

#include <objbase.h>
#include <propvarutil.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "win_util.hpp"

using winutil::Com;

// MinGW-w64 の wincodec.h には無いので自分で宣言する（Windows 7 以降の WIC にある）
#ifndef __IWICBitmapSourceTransform_INTERFACE_DEFINED__
struct IWICBitmapSourceTransform : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* prc, UINT width, UINT height, WICPixelFormatGUID* format,
                                                 WICBitmapTransformOptions transform, UINT stride, UINT bufferSize,
                                                 BYTE* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestSize(UINT* width, UINT* height) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetClosestPixelFormat(WICPixelFormatGUID* format) = 0;
    virtual HRESULT STDMETHODCALLTYPE DoesSupportTransform(WICBitmapTransformOptions transform, BOOL* supported) = 0;
};
__CRT_UUID_DECL(IWICBitmapSourceTransform, 0x3b16811b, 0x6a43, 0x4ec9, 0xb7, 0x13, 0x3d, 0x5a, 0x0c, 0x13, 0xb9, 0x40)
#endif

namespace imgwin {

namespace {

thread_local IWICImagingFactory* tlsFactory = nullptr;
thread_local bool tlsComInit = false;

IWICImagingFactory* factory() {
    if (!tlsFactory)
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tlsFactory));
    return tlsFactory;
}

// JPEG は先頭を読んで EXIF を自分で解く（WIC のメタデータ読み取りより速く、どの環境でも同じに動く）
int jpegOrientation(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    std::vector<uint8_t> head(128 * 1024);
    DWORD n = 0;
    const BOOL ok = ReadFile(h, head.data(), static_cast<DWORD>(head.size()), &n, nullptr);
    CloseHandle(h);
    return ok ? imgdupe::jpegExifOrientation(head.data(), n) : 0;
}

// EXIF の向き（1〜8）。無ければ 1
int readOrientation(IWICBitmapFrameDecode* frame, const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif")
        if (const int o = jpegOrientation(path)) return o;
    Com<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(q.put()))) return 1;
    for (const wchar_t* name : {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
        PROPVARIANT v;
        PropVariantInit(&v);
        int o = 0;
        if (SUCCEEDED(q->GetMetadataByName(name, &v))) {
            if (v.vt == VT_UI2) o = v.uiVal;
            else if (v.vt == VT_UI4) o = static_cast<int>(v.ulVal);
        }
        PropVariantClear(&v);
        if (o >= 1 && o <= 8) return o;
    }
    return 1;
}

UINT bitsPerPixel(IWICImagingFactory* wic, const WICPixelFormatGUID& fmt) {
    Com<IWICComponentInfo> info;
    Com<IWICPixelFormatInfo> pf;
    UINT bpp = 0;
    if (SUCCEEDED(wic->CreateComponentInfo(fmt, info.put())) &&
        SUCCEEDED(info->QueryInterface(IID_PPV_ARGS(pf.put()))))
        pf->GetBitsPerPixel(&bpp);
    return bpp;
}

// JPEG などデコーダーが縮小しながら読めるものは、一辺が minSide 以上で済む大きさで読む（ずっと速い）。
// 縮小できなければ frame をそのまま返す
void fastSource(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame, UINT w, UINT h, UINT minSide,
                Com<IWICBitmapSource>& out) {
    Com<IWICBitmapSourceTransform> tr;
    if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(tr.put())))) {
        UINT s = 1;
        while (s < 8 && w / (s * 2) >= minSide && h / (s * 2) >= minSide) s *= 2;
        UINT cw = (w + s - 1) / s, ch = (h + s - 1) / s;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        if (s > 1 && SUCCEEDED(tr->GetClosestSize(&cw, &ch)) && cw < w && cw && ch &&
            SUCCEEDED(tr->GetClosestPixelFormat(&fmt))) {
            const UINT bpp = bitsPerPixel(wic, fmt);
            if (bpp) {
                const UINT stride = ((cw * bpp + 31) / 32) * 4;
                std::vector<BYTE> buf(static_cast<size_t>(stride) * ch);
                Com<IWICBitmap> bmp;
                if (SUCCEEDED(tr->CopyPixels(nullptr, cw, ch, &fmt, WICBitmapTransformRotate0, stride,
                                             static_cast<UINT>(buf.size()), buf.data())) &&
                    SUCCEEDED(wic->CreateBitmapFromMemory(cw, ch, fmt, stride, static_cast<UINT>(buf.size()),
                                                          buf.data(), bmp.put()))) {
                    bmp->QueryInterface(IID_PPV_ARGS(out.put()));
                    if (out) return;
                }
            }
        }
    }
    frame->QueryInterface(IID_PPV_ARGS(out.put()));
}

bool openFrame(const std::wstring& path, Com<IWICBitmapFrameDecode>& frame, UINT& w, UINT& h) {
    IWICImagingFactory* wic = factory();
    if (!wic) return false;
    Com<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              dec.put())))
        return false;
    if (FAILED(dec->GetFrame(0, frame.put()))) return false;
    return SUCCEEDED(frame->GetSize(&w, &h)) && w && h;
}

}  // namespace

void threadBegin() {
    tlsComInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
}

void threadEnd() {
    if (tlsFactory) tlsFactory->Release();
    tlsFactory = nullptr;
    if (tlsComInit) CoUninitialize();
    tlsComInit = false;
}

imgdupe::Features decodeFeatures(const std::wstring& path) {
    using imgdupe::kThumb;
    imgdupe::Features f;
    IWICImagingFactory* wic = factory();
    Com<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (!wic || !openFrame(path, frame, w, h)) return f;
    const int orientation = readOrientation(frame.get(), path);

    Com<IWICBitmapSource> src;
    fastSource(wic, frame.get(), w, h, kThumb * 2, src);
    Com<IWICFormatConverter> conv;
    UINT sw = 0, sh = 0;
    if (!src || FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&sw, &sh)) || !sw || !sh || sw > 65535 || sh > 65535)
        return f;
    // 縮小は WIC の拡大縮小（Windows の版や環境で結果が違う）を使わず、自分で面積平均する
    imgdupe::Shrinker shrink(sw, sh);
    const UINT stride = (sw * 3 + 3) & ~3u;
    const UINT strip = std::max<UINT>(1, std::min<UINT>(sh, (4u << 20) / stride));
    std::vector<uint8_t> buf(static_cast<size_t>(stride) * strip);
    for (UINT y = 0; y < sh; y += strip) {
        const UINT rows = std::min(strip, sh - y);
        const WICRect rc{0, static_cast<INT>(y), static_cast<INT>(sw), static_cast<INT>(rows)};
        if (FAILED(conv->CopyPixels(&rc, stride, stride * rows, buf.data()))) return f;
        shrink.addRows(buf.data(), stride, y, rows, true);
    }
    const std::vector<uint8_t> px = shrink.finish();

    f = imgdupe::computeFeatures(px.data(), orientation);
    const bool swapSides = orientation >= 5;
    f.width = swapSides ? h : w;
    f.height = swapSides ? w : h;
    return f;
}

HBITMAP loadThumbnail(const std::wstring& path, int box, COLORREF bg) {
    IWICImagingFactory* wic = factory();
    Com<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (!wic || !openFrame(path, frame, w, h)) return nullptr;
    const int orientation = readOrientation(frame.get(), path);

    Com<IWICBitmapSource> src;
    fastSource(wic, frame.get(), w, h, static_cast<UINT>(box), src);
    if (!src) return nullptr;

    // 向きを直す
    static const WICBitmapTransformOptions transforms[9] = {
        WICBitmapTransformRotate0,
        WICBitmapTransformRotate0,
        WICBitmapTransformFlipHorizontal,
        WICBitmapTransformRotate180,
        WICBitmapTransformFlipVertical,
        static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate90,
        static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate270,
    };
    Com<IWICFormatConverter> conv;
    if (FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return nullptr;
    Com<IWICBitmapSource> oriented;
    Com<IWICBitmapFlipRotator> rot;
    if (orientation > 1 && SUCCEEDED(wic->CreateBitmapFlipRotator(rot.put())) &&
        SUCCEEDED(rot->Initialize(conv.get(), transforms[orientation])))
        rot->QueryInterface(IID_PPV_ARGS(oriented.put()));
    else
        conv->QueryInterface(IID_PPV_ARGS(oriented.put()));

    UINT sw = 0, sh = 0;
    oriented->GetSize(&sw, &sh);
    if (!sw || !sh) return nullptr;
    const double s = std::min(1.0, std::min(static_cast<double>(box) / sw, static_cast<double>(box) / sh));
    const UINT tw = std::max<UINT>(1, static_cast<UINT>(std::lround(sw * s)));
    const UINT th = std::max<UINT>(1, static_cast<UINT>(std::lround(sh * s)));
    Com<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(oriented.get(), tw, th, WICBitmapInterpolationModeFant)))
        return nullptr;
    std::vector<BYTE> px(static_cast<size_t>(tw) * th * 4);
    if (FAILED(scaler->CopyPixels(nullptr, tw * 4, static_cast<UINT>(px.size()), px.data()))) return nullptr;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = box;
    bi.bmiHeader.biHeight = -box;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    auto* dst = static_cast<BYTE*>(bits);
    const BYTE bb = GetBValue(bg), bgG = GetGValue(bg), br = GetRValue(bg);
    for (int i = 0; i < box * box; ++i) {
        dst[i * 4] = bb;
        dst[i * 4 + 1] = bgG;
        dst[i * 4 + 2] = br;
        dst[i * 4 + 3] = 255;
    }
    const int ox = (box - static_cast<int>(tw)) / 2, oy = (box - static_cast<int>(th)) / 2;
    for (UINT y = 0; y < th; ++y)
        for (UINT x = 0; x < tw; ++x) {
            const BYTE* p = &px[(static_cast<size_t>(y) * tw + x) * 4];
            BYTE* q = &dst[(static_cast<size_t>(oy + y) * box + ox + x) * 4];
            const int a = p[3];  // 透明な部分は背景と混ぜる（PBGRA なので色は a を掛け済み）
            for (int c = 0; c < 3; ++c) q[c] = static_cast<BYTE>(p[c] + q[c] * (255 - a) / 255);
        }
    return bmp;
}

void scanFolder(const std::wstring& root, std::vector<imgdupe::ScannedFile>& out, std::atomic<uint64_t>& found,
                const std::atomic<bool>& cancel) {
    std::vector<std::wstring> stack{root};
    while (!stack.empty() && !cancel) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        const std::wstring prefix = (dir.back() == L'\\' || dir.back() == L'/') ? dir : dir + L"\\";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((prefix + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(prefix + name);
                continue;
            }
            std::string path = winutil::narrow(prefix + name);
            if (!imgdupe::isImageExt(path)) continue;
            imgdupe::ScannedFile f;
            f.path = std::move(path);
            f.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            f.mtime = static_cast<int64_t>((static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                                           fd.ftLastWriteTime.dwLowDateTime);
            out.push_back(std::move(f));
            found.fetch_add(1, std::memory_order_relaxed);
        } while (FindNextFileW(h, &fd) && !cancel);
        FindClose(h);
    }
}

}  // namespace imgwin
