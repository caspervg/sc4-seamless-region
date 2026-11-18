#pragma once

// Pure helpers of the capture module: pixel conversion, the two clear color alpha trick, the
// projected city outline and the split of a tile into reads. No game calls, header only so
// the offline tests build them without a change to the test CMake file.
// Tests: tests/CaptureTests.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "view/ViewMath.hpp"

namespace CaptureMath
{
    // An opaque color as the snapshot reader gives it back.
    struct Rgb
    {
        uint8_t r = 0, g = 0, b = 0;
        [[nodiscard]] bool operator==(const Rgb&) const = default;
    };

    // Copies w * h pixels of an A8R8G8B8 buffer (bytes B, G, R, X) into RGBA with alpha 255.
    // The source alpha byte is not trusted: back buffers often have no alpha channel.
    inline void ConvertBgraToRgba(const uint8_t* src, const size_t srcStride, const int w, const int h, uint8_t* dst)
    {
        for (int y = 0; y < h; ++y) {
            const uint8_t* s = src + srcStride * static_cast<size_t>(y);
            const uint8_t* const end = s + static_cast<size_t>(w) * 4;
            for (; s != end; s += 4, dst += 4) {
                dst[0] = s[2];
                dst[1] = s[1];
                dst[2] = s[0];
                dst[3] = 0xff;
            }
        }
    }

    // Copies a w * h RGBA block into a larger RGBA image at (dstX, dstY).
    inline void BlitRgba(const uint8_t* src, const int w, const int h, uint8_t* dst, const int dstWidth, const int dstX,
                         const int dstY)
    {
        for (int y = 0; y < h; ++y) {
            std::memcpy(dst + (static_cast<size_t>(dstY + y) * dstWidth + dstX) * 4, src + static_cast<size_t>(y) * w * 4,
                        static_cast<size_t>(w) * 4);
        }
    }

    // How many of the pixelCount RGBA pixels have exactly the color key.
    [[nodiscard]] inline size_t CountKeyPixels(const uint8_t* rgba, const size_t pixelCount, const Rgb key)
    {
        size_t count = 0;
        const uint8_t* const end = rgba + pixelCount * 4;
        for (const uint8_t* p = rgba; p != end; p += 4) {
            count += static_cast<size_t>(p[0] == key.r && p[1] == key.g && p[2] == key.b);
        }
        return count;
    }

    // The color channel (0 = r, 1 = g, 2 = b) where the two key colors differ most, and that
    // difference (keyB - keyA, signed). The capture needs |difference| >= kMinKeyContrast.
    constexpr int kMinKeyContrast = 128;
    inline void KeyContrast(const Rgb keyA, const Rgb keyB, int& channel, int& difference)
    {
        const std::array<int, 3> d{keyB.r - keyA.r, keyB.g - keyA.g, keyB.b - keyA.b};
        channel = 0;
        for (int c = 1; c < 3; ++c) {
            if (std::abs(d[c]) > std::abs(d[channel])) {
                channel = c;
            }
        }
        difference = d[channel];
    }

    // The two clear color trick. `a` holds a render over the clear color keyA, `b` the same
    // render over keyB. A pixel that is equal in both is opaque. A pixel that follows the clear
    // color is background. In between, coverage = 1 - (b - a) / (keyB - keyA) on the channel
    // with the most contrast, and the color is taken back out of the blend with keyA.
    // Writes the result over `a` (straight alpha). Returns the count of pixels with alpha > 0.
    //
    // Differences below kOpaqueSlack of the contrast count as opaque, so render noise between
    // the two passes does not make holes. Differences above 1 - kOpaqueSlack count as empty.
    constexpr int kOpaqueSlackPercent = 6;
    inline size_t ComposeTwoPasses(uint8_t* a, const uint8_t* b, const size_t pixelCount, const Rgb keyA, const Rgb keyB)
    {
        int channel = 0;
        int contrast = 0;
        KeyContrast(keyA, keyB, channel, contrast);
        if (contrast == 0) {
            return pixelCount;
        }
        const int slack = std::max(1, std::abs(contrast) * kOpaqueSlackPercent / 100);
        const int sign = contrast > 0 ? 1 : -1;
        const int span = std::abs(contrast);
        const std::array<int, 3> key{keyA.r, keyA.g, keyA.b};

        size_t visible = 0;
        const uint8_t* const end = a + pixelCount * 4;
        for (; a != end; a += 4, b += 4) {
            // How far the pixel follows the clear color, 0..span.
            const int follow = (b[channel] - a[channel]) * sign;
            if (follow <= slack) {
                ++visible;  // a[3] is already 255
                continue;
            }
            if (follow >= span - slack) {
                a[0] = a[1] = a[2] = a[3] = 0;
                continue;
            }
            // 0 < alpha < 255: a = alpha * color + (1 - alpha) * keyA.
            const int alpha = 255 - (follow * 255 + span / 2) / span;
            for (int c = 0; c < 3; ++c) {
                const int color = ((a[c] - key[c]) * 255 + key[c] * alpha + alpha / 2) / alpha;
                a[c] = static_cast<uint8_t>(std::clamp(color, 0, 255));
            }
            a[3] = static_cast<uint8_t>(alpha);
            ++visible;
        }
        return visible;
    }

    // Turns every pixel with exactly the color key into transparent black. For a tile where
    // only one pass is known. Returns the count of pixels left.
    inline size_t ClearKeyPixels(uint8_t* rgba, const size_t pixelCount, const Rgb key)
    {
        size_t visible = 0;
        const uint8_t* const end = rgba + pixelCount * 4;
        for (uint8_t* p = rgba; p != end; p += 4) {
            if (p[0] == key.r && p[1] == key.g && p[2] == key.b) {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
            else {
                ++visible;
            }
        }
        return visible;
    }

    // -----------------------------------------------------------------------------------------
    // The outline of a city on the canvas: the convex hull of the 8 projected corners of the
    // box [0, sizeX] x [minHeight, maxHeight] x [0, sizeZ]. Everything the city can draw lies
    // inside it, so a tile outside it is empty without a render.
    // -----------------------------------------------------------------------------------------
    struct CityOutline
    {
        std::array<cS3DVector2, 8> points{};
        int count = 0;  // counter-clockwise in canvas coordinates (y down: clockwise on screen)

        [[nodiscard]] static CityOutline For(const ViewMath::OrthoProjection& projection, const float citySizeX,
                                             const float citySizeZ, const float minHeight, const float maxHeight)
        {
            std::array<cS3DVector2, 8> p{};
            int n = 0;
            for (const float h : {minHeight, maxHeight}) {
                for (const float x : {0.0f, citySizeX}) {
                    for (const float z : {0.0f, citySizeZ}) {
                        p[n++] = projection.WorldToCanvas(cS3DVector3(x, h, z));
                    }
                }
            }
            std::sort(p.begin(), p.end(), [](const cS3DVector2& l, const cS3DVector2& r) {
                return l.fX < r.fX || (l.fX == r.fX && l.fY < r.fY);
            });
            const auto cross = [](const cS3DVector2& o, const cS3DVector2& u, const cS3DVector2& v) {
                return static_cast<double>(u.fX - o.fX) * (v.fY - o.fY) - static_cast<double>(u.fY - o.fY) * (v.fX - o.fX);
            };
            // Andrew's monotone chain. 8 inputs give at most 8 hull points, plus the repeated first.
            std::array<cS3DVector2, 17> hull{};
            int k = 0;
            for (int i = 0; i < n; ++i) {
                while (k >= 2 && cross(hull[k - 2], hull[k - 1], p[i]) <= 0.0) {
                    --k;
                }
                hull[k++] = p[i];
            }
            const int lower = k + 1;
            for (int i = n - 2; i >= 0; --i) {
                while (k >= lower && cross(hull[k - 2], hull[k - 1], p[i]) <= 0.0) {
                    --k;
                }
                hull[k++] = p[i];
            }
            CityOutline outline;
            outline.count = std::min(k - 1, 8);
            std::copy_n(hull.begin(), outline.count, outline.points.begin());
            return outline;
        }

        // The rectangle [x0, x1) x [y0, y1) touches the outline (separating axis test).
        [[nodiscard]] bool Intersects(const float x0, const float y0, const float x1, const float y1) const
        {
            if (count < 3) {
                return true;
            }
            float minX = points[0].fX, maxX = minX, minY = points[0].fY, maxY = minY;
            for (int i = 1; i < count; ++i) {
                minX = std::min(minX, points[i].fX);
                maxX = std::max(maxX, points[i].fX);
                minY = std::min(minY, points[i].fY);
                maxY = std::max(maxY, points[i].fY);
            }
            if (x1 <= minX || x0 >= maxX || y1 <= minY || y0 >= maxY) {
                return false;
            }
            const std::array<cS3DVector2, 4> corners{cS3DVector2(x0, y0), cS3DVector2(x1, y0), cS3DVector2(x1, y1),
                                                     cS3DVector2(x0, y1)};
            for (int i = 0; i < count; ++i) {
                const cS3DVector2& p = points[i];
                const cS3DVector2& q = points[(i + 1) % count];
                // The hull is on the left of every edge. If all rectangle corners are on the
                // right of one edge, that edge separates them.
                bool allOutside = true;
                for (const cS3DVector2& c : corners) {
                    const double side =
                        static_cast<double>(q.fX - p.fX) * (c.fY - p.fY) - static_cast<double>(q.fY - p.fY) * (c.fX - p.fX);
                    if (side > 0.0) {
                        allOutside = false;
                        break;
                    }
                }
                if (allOutside) {
                    return false;
                }
            }
            return true;
        }
    };

    // -----------------------------------------------------------------------------------------
    // One read of a part of a tile. A tile needs more than one when the game window is smaller
    // than the tile.
    // -----------------------------------------------------------------------------------------
    struct TileRead
    {
        int x = 0, y = 0;  // top-left of the part inside the tile
        int w = 0, h = 0;
    };

    // Splits a tileSize x tileSize tile into parts of at most viewportW x viewportH, row by row.
    // Reuses the capacity of `reads`. Empty when the viewport has no size.
    inline void PlanTileReads(const int tileSize, const int viewportW, const int viewportH, std::vector<TileRead>& reads)
    {
        reads.clear();
        const int partW = std::min(tileSize, viewportW);
        const int partH = std::min(tileSize, viewportH);
        if (tileSize <= 0 || partW <= 0 || partH <= 0) {
            return;
        }
        for (int y = 0; y < tileSize; y += partH) {
            for (int x = 0; x < tileSize; x += partW) {
                reads.push_back({x, y, std::min(partW, tileSize - x), std::min(partH, tileSize - y)});
            }
        }
    }

    // The view target that makes the game draw the canvas point `canvasTopLeft` at the screen
    // pixel (screenX, screenY). `canvasTopLeft` must lie on the game's pixel grid (base corner
    // plus whole pixels, as ViewMath::TileGrid gives). The game floors the projected base corner
    // (UpdateCameraPositionParams 0x007cc290): the target aims at the middle of that pixel, so
    // float error cannot move the result to a neighbor pixel.
    [[nodiscard]] inline cS3DVector3 TargetForCanvasAt(const ViewMath::OrthoProjection& projection,
                                                       const cS3DVector2& canvasTopLeft, const int screenX,
                                                       const int screenY, const int viewportW, const int viewportH)
    {
        // screen = canvas + floor(viewport / 2 + base - target) - base, see docs/re-notes/viewmath.md.
        const cS3DVector2 targetCanvas{canvasTopLeft.fX + static_cast<float>(viewportW) * 0.5f - 0.5f - static_cast<float>(screenX),
                                       canvasTopLeft.fY + static_cast<float>(viewportH) * 0.5f - 0.5f - static_cast<float>(screenY)};
        return projection.CanvasToGround(targetCanvas, 0.0f);
    }
}
