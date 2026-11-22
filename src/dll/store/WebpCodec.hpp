#pragma once

// Store: lossy WebP with alpha through libwebp. Pure functions, safe on any thread.
//
// Encoder settings: lossy, method 3 (one step faster than the libwebp default 4, about 3 percent
// bigger files), alpha kept at quality 100 with alpha compression on, so the edge of a city stays
// clean while the colors are lossy. "exact" is off: the color under fully transparent pixels is
// not kept. Every call is independent, no shared state.

#include <cstdint>
#include <span>
#include <vector>

namespace WebpCodec
{
    // Largest side libwebp accepts.
    inline constexpr int kMaxDimension = 16383;

    // rgba is w * h * 4 bytes, straight (not premultiplied), top row first. quality 0..100
    // (clamped). Replaces out. Returns false on bad arguments or an encoder error.
    bool Encode(const uint8_t* rgba, int w, int h, int quality, std::vector<uint8_t>& out);

    // Reads only the header. False when data is not a WebP image.
    bool GetSize(std::span<const uint8_t> data, int& w, int& h);

    // Replaces rgba with w * h * 4 bytes (straight RGBA, top row first).
    bool Decode(std::span<const uint8_t> data, std::vector<uint8_t>& rgba, int& w, int& h);

    // Decodes into a caller buffer, no allocation. Sets w and h whenever the header is valid.
    // Returns false when data is bad or capacityBytes is smaller than w * h * 4 (w and h then
    // tell the needed size).
    bool DecodeInto(std::span<const uint8_t> data, uint8_t* rgba, int capacityBytes, int& w, int& h);
}
