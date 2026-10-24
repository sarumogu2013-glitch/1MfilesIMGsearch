#include "image_win.hpp"

#include <objbase.h>
#include <propvarutil.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <mutex>

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

namespace {

// box×box に収まるよう縮めた 32bpp PBGRA の画素（大きくはしない）
bool fittedPixels(IWICImagingFactory* wic, IWICBitmapSource* src, UINT box, std::vector<BYTE>& px, UINT& tw, UINT& th) {
    UINT sw = 0, sh = 0;
    src->GetSize(&sw, &sh);
    if (!sw || !sh) return false;
    const double s = std::min(1.0, std::min(static_cast<double>(box) / sw, static_cast<double>(box) / sh));
    tw = std::max<UINT>(1, static_cast<UINT>(std::lround(sw * s)));
    th = std::max<UINT>(1, static_cast<UINT>(std::lround(sh * s)));
    Com<IWICFormatConverter> conv;
    Com<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateFormatConverter(conv.put())) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapScaler(scaler.put())) ||
        FAILED(scaler->Initialize(conv.get(), tw, th, WICBitmapInterpolationModeFant)))
        return false;
    px.resize(static_cast<size_t>(tw) * th * 4);
    return SUCCEEDED(scaler->CopyPixels(nullptr, tw * 4, static_cast<UINT>(px.size()), px.data()));
}

// ファイルを読み、box×box に収まるよう縮めて、EXIF の向きを直した画素
// （回転は自分で行う。WIC の回転は環境によって動かないため）
bool orientedPixels(IWICImagingFactory* wic, const std::wstring& path, UINT box, std::vector<BYTE>& px, UINT& w,
                    UINT& h) {
    Com<IWICBitmapFrameDecode> frame;
    UINT fw = 0, fh = 0;
    if (!openFrame(path, frame, fw, fh)) return false;
    const int orientation = readOrientation(frame.get(), path);
    Com<IWICBitmapSource> src;
    fastSource(wic, frame.get(), fw, fh, box, src);
    if (!src || !fittedPixels(wic, src.get(), box, px, w, h)) return false;
    uint32_t ow = w, oh = h;
    imgdupe::orientPixels(px, ow, oh, 4, orientation);
    w = ow;
    h = oh;
    return true;
}

// 画素を box×box の DIB の中央に置く（背景は bg）
HBITMAP composeThumb(const std::vector<BYTE>& px, UINT tw, UINT th, int box, COLORREF bg) {
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

}  // namespace

HBITMAP loadThumbnail(const std::wstring& path, int box, COLORREF bg) {
    IWICImagingFactory* wic = factory();
    std::vector<BYTE> px;
    UINT w = 0, h = 0;
    if (!wic || !orientedPixels(wic, path, static_cast<UINT>(box), px, w, h)) return nullptr;
    return composeThumb(px, w, h, box, bg);
}

HBITMAP thumbFromBytes(const std::vector<uint8_t>& bytes, int box, COLORREF bg) {
    IWICImagingFactory* wic = factory();
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    if (!wic || bytes.empty() || FAILED(wic->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put())) ||
        FAILED(dec->GetFrame(0, frame.put())))
        return nullptr;
    std::vector<BYTE> px;
    UINT w = 0, h = 0;
    if (!fittedPixels(wic, frame.get(), static_cast<UINT>(box), px, w, h)) return nullptr;
    return composeThumb(px, w, h, box, bg);
}

bool encodeThumbnail(const std::wstring& path, int maxSide, std::vector<uint8_t>& jpeg, int& width, int& height) {
    IWICImagingFactory* wic = factory();
    std::vector<BYTE> px;
    UINT tw = 0, th = 0;
    if (!wic || !orientedPixels(wic, path, static_cast<UINT>(maxSide), px, tw, th)) return false;
    // 透明な部分は白にする（JPEG はアルファを持てない）
    std::vector<BYTE> bgr(static_cast<size_t>(tw) * th * 3);
    for (size_t i = 0; i < static_cast<size_t>(tw) * th; ++i)
        for (int c = 0; c < 3; ++c) bgr[i * 3 + c] = static_cast<BYTE>(px[i * 4 + c] + (255 - px[i * 4 + 3]));

    Com<IStream> stream;
    Com<IWICBitmapEncoder> enc;
    Com<IWICBitmapFrameEncode> fe;
    Com<IPropertyBag2> props;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, enc.put())) ||
        FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(fe.put(), props.put())))
        return false;
    PROPBAG2 opt{};
    opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_R4;
    v.fltVal = 0.82f;
    props->Write(1, &opt, &v);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(fe->Initialize(props.get())) || FAILED(fe->SetSize(tw, th)) || FAILED(fe->SetPixelFormat(&fmt)) ||
        !IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) ||
        FAILED(fe->WritePixels(th, tw * 3, static_cast<UINT>(bgr.size()), bgr.data())) || FAILED(fe->Commit()) ||
        FAILED(enc->Commit()))
        return false;
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return false;
    jpeg.resize(static_cast<size_t>(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ULONG read = 0;
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(stream->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &read)) || read != jpeg.size())
        return false;
    width = static_cast<int>(tw);
    height = static_cast<int>(th);
    return true;
}

namespace {

std::string propString(const PROPVARIANT& v) {
    if (v.vt == VT_LPWSTR && v.pwszVal) return winutil::narrow(v.pwszVal);
    if (v.vt == VT_BSTR && v.bstrVal) return winutil::narrow(v.bstrVal);
    if (v.vt == VT_LPSTR && v.pszVal) return v.pszVal;
    return {};
}

std::vector<std::string> propStrings(const PROPVARIANT& v) {
    std::vector<std::string> out;
    if (v.vt == (VT_VECTOR | VT_LPWSTR)) {
        for (ULONG i = 0; i < v.calpwstr.cElems; ++i)
            if (v.calpwstr.pElems[i]) out.push_back(winutil::narrow(v.calpwstr.pElems[i]));
    } else if (v.vt == (VT_VECTOR | VT_LPSTR)) {
        for (ULONG i = 0; i < v.calpstr.cElems; ++i)
            if (v.calpstr.pElems[i]) out.emplace_back(v.calpstr.pElems[i]);
    } else {
        // 1 つの文字で「a; b; c」と入っていることもある
        const std::string s = propString(v);
        size_t a = 0;
        while (a < s.size()) {
            size_t b = s.find(';', a);
            if (b == std::string::npos) b = s.size();
            std::string t = s.substr(a, b - a);
            while (!t.empty() && t.front() == ' ') t.erase(t.begin());
            while (!t.empty() && t.back() == ' ') t.pop_back();
            if (!t.empty()) out.push_back(t);
            a = b + 1;
        }
    }
    return out;
}

double propNumber(const PROPVARIANT& v) {
    switch (v.vt) {
        case VT_R8: return v.dblVal;
        case VT_R4: return v.fltVal;
        case VT_UI1: return v.bVal;
        case VT_UI2: return v.uiVal;
        case VT_UI4: return v.ulVal;
        case VT_I2: return v.iVal;
        case VT_I4: return v.lVal;
        case VT_UI8: return static_cast<double>(v.uhVal.QuadPart);
        default: return 0;
    }
}

// クエリの名前を順に試し、最初に見つかったものを fn に渡す
template <class Fn>
bool query(IWICMetadataQueryReader* q, std::initializer_list<const wchar_t*> names, Fn fn) {
    for (const wchar_t* name : names) {
        PROPVARIANT v;
        PropVariantInit(&v);
        bool used = false;
        if (SUCCEEDED(q->GetMetadataByName(name, &v)) && v.vt != VT_EMPTY) used = fn(v);
        PropVariantClear(&v);
        if (used) return true;
    }
    return false;
}

std::string containerName(const GUID& g) {
    if (IsEqualGUID(g, GUID_ContainerFormatJpeg)) return "JPEG";
    if (IsEqualGUID(g, GUID_ContainerFormatPng)) return "PNG";
    if (IsEqualGUID(g, GUID_ContainerFormatGif)) return "GIF";
    if (IsEqualGUID(g, GUID_ContainerFormatBmp)) return "BMP";
    if (IsEqualGUID(g, GUID_ContainerFormatTiff)) return "TIFF";
    if (IsEqualGUID(g, GUID_ContainerFormatWmp)) return "JPEG XR";
    if (IsEqualGUID(g, GUID_ContainerFormatIco)) return "ICO";
    // WebP・HEIF は新しい SDK にしか定義が無いので値で比べる
    static const GUID webp = {0xe094b0e2, 0x67f2, 0x45b3, {0xb0, 0xea, 0x11, 0x53, 0x37, 0xca, 0x7c, 0xf3}};
    static const GUID heif = {0xe1e62521, 0x6787, 0x405b, {0xa3, 0x39, 0x50, 0x07, 0x15, 0xb5, 0x76, 0x3f}};
    if (IsEqualGUID(g, webp)) return "WebP";
    if (IsEqualGUID(g, heif)) return "HEIF";
    return "";
}

std::vector<uint8_t> readHead(const std::wstring& path, size_t max) {
    std::vector<uint8_t> buf;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return buf;
    buf.resize(max);
    DWORD n = 0;
    if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) n = 0;
    buf.resize(n);
    CloseHandle(h);
    return buf;
}

}  // namespace

bool readFileMeta(const std::wstring& path, imgdupe::FileMeta& m) {
    using imgdupe::fileTimeToUnix;
    // NTFS: 作成・アクセス日時、属性、ハードリンクの数、ファイル ID（移動・名前変更しても変わらない）
    {
        HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        BY_HANDLE_FILE_INFORMATION fi;
        if (GetFileInformationByHandle(h, &fi)) {
            auto ft = [](const FILETIME& t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
            m.created = fileTimeToUnix(ft(fi.ftCreationTime));
            m.accessed = fileTimeToUnix(ft(fi.ftLastAccessTime));
            m.attributes = fi.dwFileAttributes;
            m.hardlinks = fi.nNumberOfLinks;
            m.fileId = (static_cast<uint64_t>(fi.nFileIndexHigh) << 32) | fi.nFileIndexLow;
        }
        CloseHandle(h);
    }
    // 代替データストリーム Zone.Identifier（ブラウザでダウンロードした時の取得元）
    {
        const std::vector<uint8_t> z = readHead(path + L":Zone.Identifier", 8192);
        if (!z.empty()) imgdupe::parseZoneIdentifier(std::string(z.begin(), z.end()), m.zoneUrl, m.zoneReferrer);
    }

    // JPEG と PNG の EXIF・XMP・テキストは自分で読む（どの環境でも同じに読める）
    const std::vector<uint8_t> head = readHead(path, 4 * 1024 * 1024);
    imgdupe::PngInfo pi;
    if (!imgdupe::parseJpegMeta(head.data(), head.size(), m) && imgdupe::parsePng(head.data(), head.size(), pi))
        imgdupe::applyPng(pi, m);

    // ほかの形式（TIFF・HEIF など）や、足りない項目は Windows の写真のプロパティで埋める
    imgdupe::FileMeta w;
    IWICImagingFactory* wic = factory();
    Com<IWICBitmapDecoder> dec;
    if (!wic || FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, dec.put())))
        return true;  // 画像として読めなくても、ファイルの情報はある
    GUID container{};
    if (SUCCEEDED(dec->GetContainerFormat(&container))) w.format = containerName(container);
    UINT frames = 0;
    if (SUCCEEDED(dec->GetFrameCount(&frames)) && frames > 1) w.animated = true;
    Com<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(dec->GetFrame(0, frame.put()))) {
        WICPixelFormatGUID fmt{};
        if (SUCCEEDED(frame->GetPixelFormat(&fmt))) w.bitDepth = static_cast<int>(bitsPerPixel(wic, fmt));
        Com<IWICMetadataQueryReader> q;
        if (SUCCEEDED(frame->GetMetadataQueryReader(q.put()))) {
            query(q.get(), {L"System.Photo.DateTaken"}, [&](const PROPVARIANT& v) {
                if (v.vt != VT_FILETIME) return false;
                w.taken = fileTimeToUnix((static_cast<uint64_t>(v.filetime.dwHighDateTime) << 32) | v.filetime.dwLowDateTime);
                return w.taken != 0;
            });
            std::string maker, model;
            query(q.get(), {L"System.Photo.CameraManufacturer"}, [&](const PROPVARIANT& v) { return !(maker = propString(v)).empty(); });
            query(q.get(), {L"System.Photo.CameraModel"}, [&](const PROPVARIANT& v) { return !(model = propString(v)).empty(); });
            w.camera = maker.empty() || model.compare(0, maker.size(), maker) == 0 ? model : maker + " " + model;
            query(q.get(), {L"System.Photo.LensModel"}, [&](const PROPVARIANT& v) { return !(w.lens = propString(v)).empty(); });
            query(q.get(), {L"System.Photo.FocalLength"}, [&](const PROPVARIANT& v) { return (w.focal = propNumber(v)) > 0; });
            query(q.get(), {L"System.Photo.ISOSpeed"}, [&](const PROPVARIANT& v) { return (w.iso = static_cast<int>(propNumber(v))) > 0; });
            w.hasGps = query(q.get(), {L"System.GPS.Latitude"}, [](const PROPVARIANT&) { return true; });
            query(q.get(), {L"System.ApplicationName"}, [&](const PROPVARIANT& v) { return !(w.software = propString(v)).empty(); });
            query(q.get(), {L"System.Keywords"}, [&](const PROPVARIANT& v) { return !(w.keywords = propStrings(v)).empty(); });
            query(q.get(), {L"System.Rating"}, [&](const PROPVARIANT& v) {
                // 0〜99 → 星 0〜5（Windows と同じ区切り）
                const int r = static_cast<int>(propNumber(v));
                w.rating = r <= 0 ? 0 : r < 13 ? 1 : r < 38 ? 2 : r < 63 ? 3 : r < 88 ? 4 : 5;
                return true;
            });
            query(q.get(), {L"System.Title"}, [&](const PROPVARIANT& v) { return !(w.title = propString(v)).empty(); });
            query(q.get(), {L"System.Subject"}, [&](const PROPVARIANT& v) { return !(w.subject = propString(v)).empty(); });
            query(q.get(), {L"System.Comment"}, [&](const PROPVARIANT& v) { return !(w.comment = propString(v)).empty(); });
            query(q.get(), {L"System.Author"}, [&](const PROPVARIANT& v) {
                for (const auto& a : propStrings(v)) w.author += (w.author.empty() ? "" : "; ") + a;
                return !w.author.empty();
            });
        }
    }
    m.bitDepth = w.bitDepth;  // 色深度は WIC の値を使う
    imgdupe::fillMissing(m, w);
    return true;
}

namespace {
std::mutex excludeMutex;
std::vector<std::wstring> excludeDirs;  // 小文字

std::wstring lowerPath(std::wstring s) {
    for (auto& c : s) c = c == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(c));
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}
}  // namespace

void setScanExcludes(const std::vector<std::string>& dirs) {
    std::lock_guard<std::mutex> lock(excludeMutex);
    excludeDirs.clear();
    for (const auto& d : dirs) excludeDirs.push_back(lowerPath(winutil::widen(d)));
}

void scanFolder(const std::wstring& root, std::vector<imgdupe::ScannedFile>& out, std::atomic<uint64_t>& found,
                const std::atomic<bool>& cancel) {
    std::vector<std::wstring> excludes;
    {
        std::lock_guard<std::mutex> lock(excludeMutex);
        excludes = excludeDirs;
    }
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
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                // ハードリンクの展開先（中身は元画像と同じファイルなので、読むと重複になる）
                if (_wcsicmp(name.c_str(), L"_vfolders") == 0) continue;
                const std::wstring sub = prefix + name;
                if (!excludes.empty() && std::find(excludes.begin(), excludes.end(), lowerPath(sub)) != excludes.end()) continue;
                stack.push_back(sub);
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
