#pragma once

// The capture's view of the camera module (camera/CameraExtender, W2-B) and of the two static
// time helpers of the store (store/TileStore, W2-D).
//
// The capture was written in parallel with those modules, against these final signatures:
//     void     CameraExtender::SetFreeRoam(bool enabled);            // counted: true, then false
//     uint32_t CameraExtender::SetEdgeWallsHidden(uint32_t mask);    // returns the mask before
//     void     CameraExtender::SetOutsideClearColor(uint32_t rgba);  // 0xRRGGBBAA
//     void     CameraExtender::RestoreOutsideClearColor();
//     void     CameraExtender::RefreshNeighborWalls();
//     bool     NeighborRenderer::Enabled() const;
//     static int64_t TileStore::FileTime(const std::filesystem::path&);
//     static int64_t TileStore::NowFileTime();
//
// Each wrapper calls the method when the class has it and reports false when it does not, so
// the capture compiles and works against a header with or without them. The capturer has its
// own fallback for each (docs/re-notes/capture.md, "Fallbacks").

#include <concepts>
#include <cstdint>
#include <filesystem>

#include "camera/CameraExtender.hpp"
#include "store/TileStore.hpp"

namespace CameraAdapter
{
    template <typename T>
    concept HasFreeRoam = requires(T& camera) { camera.SetFreeRoam(true); };
    template <typename T>
    concept HasOutsideClearColor = requires(T& camera) {
        camera.SetOutsideClearColor(uint32_t{});
        camera.RestoreOutsideClearColor();
    };
    template <typename T>
    concept ReturnsWallMask = requires(T& camera) {
        { camera.SetEdgeWallsHidden(uint32_t{}) } -> std::convertible_to<uint32_t>;
    };
    template <typename T>
    concept HasRefreshNeighborWalls = requires(T& camera) { camera.RefreshNeighborWalls(); };
    template <typename T>
    concept HasFileTime = requires(const std::filesystem::path& file) {
        { T::FileTime(file) } -> std::convertible_to<int64_t>;
        { T::NowFileTime() } -> std::convertible_to<int64_t>;
    };

    template <typename T>
    bool SetFreeRoam(T* camera, const bool on)
    {
        if constexpr (HasFreeRoam<T>) {
            if (camera) {
                camera->SetFreeRoam(on);
                return true;
            }
        }
        return false;
    }

    // Hides the walls of the edges in `mask`. Returns the mask to give back at restore: the
    // one before this call, or 0 when the camera module does not tell.
    template <typename T>
    uint32_t SetEdgeWallsHidden(T* camera, const uint32_t mask)
    {
        if (!camera) {
            return 0;
        }
        if constexpr (ReturnsWallMask<T>) {
            return camera->SetEdgeWallsHidden(mask);
        }
        else {
            camera->SetEdgeWallsHidden(mask);
            return 0;
        }
    }

    template <typename T>
    bool SetOutsideClearColor(T* camera, const uint32_t rgba)
    {
        if constexpr (HasOutsideClearColor<T>) {
            if (camera) {
                camera->SetOutsideClearColor(rgba);
                return true;
            }
        }
        return false;
    }

    template <typename T>
    bool RestoreOutsideClearColor(T* camera)
    {
        if constexpr (HasOutsideClearColor<T>) {
            if (camera) {
                camera->RestoreOutsideClearColor();
                return true;
            }
        }
        return false;
    }

    // Lets the camera module see the new render of this city for its automatic wall choice.
    template <typename T>
    void RefreshNeighborWalls(T* camera)
    {
        if constexpr (HasRefreshNeighborWalls<T>) {
            if (camera) {
                camera->RefreshNeighborWalls();
            }
        }
    }

    // NeighborRenderer::Enabled() (render module), or true when the header does not have it.
    template <typename T>
    bool NeighborsEnabled(const T* renderer)
    {
        if constexpr (requires(const T& r) { { r.Enabled() } -> std::convertible_to<bool>; }) {
            return !renderer || renderer->Enabled();
        }
        else {
            return true;
        }
    }

    // The store's own time readings, so CityRenderMeta times match TileStore::IsStale exactly.
    // False when the store header does not have them.
    template <typename T = TileStore>
    bool StoreTimes(const std::filesystem::path& saveFile, int64_t& saveFileTime, int64_t& now)
    {
        if constexpr (HasFileTime<T>) {
            saveFileTime = T::FileTime(saveFile);
            now = T::NowFileTime();
            return true;
        }
        else {
            return false;
        }
    }
}
