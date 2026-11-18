#pragma once

// Store (W2-D): lossy WebP with alpha through libwebp. Pure functions, safe on any thread.

#include <cstdint>
#include <span>
#include <vector>

namespace WebpCodec
{
    // rgba is w * h * 4 bytes, top row first. quality 0..100. Replaces out.
    bool Encode(const uint8_t* rgba, int w, int h, int quality, std::vector<uint8_t>& out);
    // Replaces rgba with w * h * 4 bytes, top row first.
    bool Decode(std::span<const uint8_t> data, std::vector<uint8_t>& rgba, int& w, int& h);
}
