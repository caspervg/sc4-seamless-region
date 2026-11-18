#include "store/WebpCodec.hpp"

#include <algorithm>

#include <webp/decode.h>
#include <webp/encode.h>

namespace WebpCodec
{
    bool Encode(const uint8_t* rgba, const int w, const int h, const int quality, std::vector<uint8_t>& out)
    {
        out.clear();
        if (!rgba || w <= 0 || h <= 0 || w > kMaxDimension || h > kMaxDimension) {
            return false;
        }

        WebPConfig config;
        if (!WebPConfigInit(&config)) {
            return false;
        }
        config.quality = static_cast<float>(std::clamp(quality, 0, 100));
        config.method = 3;
        config.alpha_compression = 1;
        config.alpha_quality = 100;
        config.exact = 0;
        if (!WebPValidateConfig(&config)) {
            return false;
        }

        WebPPicture picture;
        if (!WebPPictureInit(&picture)) {
            return false;
        }
        picture.use_argb = 0;  // lossy, so YUVA planes
        picture.width = w;
        picture.height = h;
        if (!WebPPictureImportRGBA(&picture, rgba, w * 4)) {
            WebPPictureFree(&picture);
            return false;
        }

        WebPMemoryWriter writer;
        WebPMemoryWriterInit(&writer);
        picture.writer = WebPMemoryWrite;
        picture.custom_ptr = &writer;

        const bool ok = WebPEncode(&config, &picture) != 0;
        if (ok) {
            out.assign(writer.mem, writer.mem + writer.size);
        }
        WebPMemoryWriterClear(&writer);
        WebPPictureFree(&picture);
        return ok;
    }

    bool GetSize(const std::span<const uint8_t> data, int& w, int& h)
    {
        w = 0;
        h = 0;
        return !data.empty() && WebPGetInfo(data.data(), data.size(), &w, &h) != 0;
    }

    bool DecodeInto(const std::span<const uint8_t> data, uint8_t* rgba, const int capacityBytes, int& w, int& h)
    {
        if (!GetSize(data, w, h)) {
            return false;
        }
        const size_t needed = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        if (!rgba || capacityBytes < 0 || static_cast<size_t>(capacityBytes) < needed) {
            return false;
        }
        return WebPDecodeRGBAInto(data.data(), data.size(), rgba, needed, w * 4) != nullptr;
    }

    bool Decode(const std::span<const uint8_t> data, std::vector<uint8_t>& rgba, int& w, int& h)
    {
        rgba.clear();
        if (!GetSize(data, w, h)) {
            return false;
        }
        const size_t needed = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        rgba.resize(needed);
        if (!DecodeInto(data, rgba.data(), static_cast<int>(std::min<size_t>(needed, 0x7fffffff)), w, h)) {
            rgba.clear();
            return false;
        }
        return true;
    }
}
