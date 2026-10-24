#include "core/gfx/room_codec.h"

#include <windows.h>
#include <wincodec.h>

#include <cstring>

namespace bvr::room_codec {
namespace {

// COM for the calling thread, kept for the call (whatever apartment the thread already has is fine for WIC).
struct Com {
    bool mine = false;
    Com() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mine = hr == S_OK || hr == S_FALSE;
    }
    ~Com() {
        if (mine) CoUninitialize();
    }
};

template <class T>
void rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

IWICImagingFactory* factory() {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return nullptr;
    return f;
}

bool stream_bytes(IStream* s, std::vector<uint8_t>& out) {
    STATSTG st{};
    if (FAILED(s->Stat(&st, STATFLAG_NONAME))) return false;
    out.resize(static_cast<size_t>(st.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    s->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG got = 0;
    return SUCCEEDED(s->Read(out.data(), static_cast<ULONG>(out.size()), &got)) && got == out.size();
}

bool encode(const GUID& container, const GUID& fmt, const void* pixels, uint32_t stride, uint32_t w, uint32_t h,
            bool jpeg, std::vector<uint8_t>& out) {
    Com com;
    IWICImagingFactory* f = factory();
    if (!f) return false;
    IStream* s = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &s)) && SUCCEEDED(f->CreateEncoder(container, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, &props));
    if (ok && jpeg && props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.92f;
        props->Write(1, &opt, &v);
    }
    WICPixelFormatGUID pf = fmt;
    ok = ok && SUCCEEDED(fr->Initialize(props)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&pf)) &&
         IsEqualGUID(pf, fmt) &&
         SUCCEEDED(fr->WritePixels(h, stride, stride * h, static_cast<BYTE*>(const_cast<void*>(pixels)))) &&
         SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()) && stream_bytes(s, out);
    rel(props);
    rel(fr);
    rel(enc);
    rel(s);
    rel(f);
    return ok;
}

bool decode(const uint8_t* bytes, size_t n, const GUID& fmt, uint32_t w, uint32_t h, uint32_t stride, void* pixels) {
    Com com;
    IWICImagingFactory* f = factory();
    if (!f) return false;
    IWICStream* s = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    UINT fw = 0, fh = 0;
    bool ok = SUCCEEDED(f->CreateStream(&s)) &&
              SUCCEEDED(s->InitializeFromMemory(const_cast<BYTE*>(bytes), static_cast<DWORD>(n))) &&
              SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fr->GetSize(&fw, &fh)) && fw == w && fh == h &&
              SUCCEEDED(f->CreateFormatConverter(&cv)) &&
              SUCCEEDED(cv->Initialize(fr, fmt, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(cv->CopyPixels(nullptr, stride, stride * h, static_cast<BYTE*>(pixels)));
    rel(cv);
    rel(fr);
    rel(dec);
    rel(s);
    rel(f);
    return ok;
}

} // namespace

bool encode_colour(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    std::vector<uint8_t> bgr(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
        bgr[i * 3 + 0] = rgba[i * 4 + 2];
        bgr[i * 3 + 1] = rgba[i * 4 + 1];
        bgr[i * 3 + 2] = rgba[i * 4 + 0];
    }
    return encode(GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, bgr.data(), w * 3, w, h, true, out);
}

bool decode_colour(const uint8_t* bytes, size_t n, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba) {
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    if (!decode(bytes, n, GUID_WICPixelFormat32bppRGBA, w, h, w * 4, rgba.data())) return false;
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return true;
}

bool encode_depth(const float* metres, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    std::vector<uint16_t> mm(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < mm.size(); ++i) {
        const float v = metres[i] * 1000.0f;
        mm[i] = static_cast<uint16_t>(v <= 0.0f ? 0.0f : v >= 65535.0f ? 65535.0f : v + 0.5f);
    }
    return encode(GUID_ContainerFormatPng, GUID_WICPixelFormat16bppGray, mm.data(), w * 2, w, h, false, out);
}

bool decode_depth(const uint8_t* bytes, size_t n, uint32_t w, uint32_t h, std::vector<float>& metres) {
    std::vector<uint16_t> mm(static_cast<size_t>(w) * h);
    if (!decode(bytes, n, GUID_WICPixelFormat16bppGray, w, h, w * 2, mm.data())) return false;
    metres.resize(mm.size());
    for (size_t i = 0; i < mm.size(); ++i) metres[i] = mm[i] * 0.001f;
    return true;
}

} // namespace bvr::room_codec
