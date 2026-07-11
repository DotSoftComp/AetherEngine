#include "image.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <cstdio>

namespace ae {

namespace {

// One-time COM + WIC factory init for the loading thread.
IWICImagingFactory* wicFactory() {
    static IWICImagingFactory* factory = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* f = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&f));
        if (FAILED(hr)) {
            std::fprintf(stderr, "[Image] WIC factory creation failed (0x%08lx)\n", hr);
            return (IWICImagingFactory*)nullptr;
        }
        return f;
    }();
    return factory;
}

} // namespace

bool decodeImage(const uint8_t* bytes, size_t size, ImageData& out) {
    IWICImagingFactory* factory = wicFactory();
    if (!factory) return false;

    IStream* stream = SHCreateMemStream(bytes, (UINT)size);
    if (!stream) return false;

    bool ok = false;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    do {
        if (FAILED(factory->CreateDecoderFromStream(stream, nullptr,
                                                    WICDecodeMetadataCacheOnDemand, &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)))
            break;

        UINT w = 0, h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) break;

        out.width = (int)w;
        out.height = (int)h;
        out.rgba.resize((size_t)w * h * 4);
        if (FAILED(converter->CopyPixels(nullptr, w * 4, (UINT)out.rgba.size(), out.rgba.data())))
            break;
        ok = true;
    } while (false);

    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    stream->Release();
    return ok;
}

} // namespace ae
