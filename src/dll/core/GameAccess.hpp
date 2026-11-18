#pragma once

// Null-safe access to the game objects, the shared ModuleContext and the Module lifecycle.
//
// Every accessor returns nullptr (or -1) when the object does not exist at the moment, for
// example the city objects in the region view. Pointers are non-owning: do not Release them
// and do not keep them across a city change (OnCityShutdown). Game thread only.

#include <cstdint>
#include <filesystem>

class cIGZDrawService;
class cIGZGDriver;
class cIGZImGuiService;
class cIGZS3DCameraService;
class cIGZWorldDrawService;
class cISC43DRender;
class cISC4App;
class cISC4City;
class cISC4Region;
class cISC4RegionalCity;
class cISC4View3DWin;
class cSC4CameraControl;

class CameraExtender;
class CameraReader;
class CityCapturer;
class CitySwitcher;
class HtmlMapExporter;
class IdleScheduler;
class MapOverlay;
class MessageBus;
class ModelLoadMonitor;
class NeighborPicker;
class NeighborRenderer;
class RegionBatchRenderer;
class RegionIndex;
class Settings;
class SnapshotReader;
class TileCache;
class TileStore;

namespace GameAccess
{
    // SC4VersionDetection reports 641 (the only build with known addresses).
    [[nodiscard]] bool IsSupportedGame();
    [[nodiscard]] uint16_t GameVersion();

    // Director only: caches the app at PostAppInit and drops it at shutdown. The city view
    // objects (View3D, Renderer, CameraControl) are only returned between OnCityInit (before
    // the modules' OnCityInit) and OnCityShutdown (after the modules' OnCityShutdown). So
    // they are nullptr from CloseCity until the new city's kPostCityInit arrives, also when
    // City() already returns the new city.
    void Initialize();
    void Shutdown();
    void OnCityInit();
    void OnCityShutdown();

    [[nodiscard]] cISC4App* App();
    [[nodiscard]] cISC4City* City();
    [[nodiscard]] cISC4Region* Region();
    // The cISC4RegionalCity of the city being played. Freed by CloseCity: never keep it.
    [[nodiscard]] cISC4RegionalCity* RegionalCity();
    // The city view window. Only exists while a city is loaded.
    [[nodiscard]] cISC4View3DWin* View3D();
    [[nodiscard]] cISC43DRender* Renderer();
    // cSC43DRender::GetCameraControl (renderer + 0x8c). Read its fields with the offsets in
    // GameAddresses::CameraControl, or use CameraReader.
    [[nodiscard]] cSC4CameraControl* CameraControl();
    // The graphics driver the renderer draws with (mpGDriver 0x00b4e1ec, fallback spGDriver
    // 0x00b43ca4).
    [[nodiscard]] cIGZGDriver* GDriver();
    // 0..4 for zoom 1..5, or -1 without a city view.
    [[nodiscard]] int Zoom();
    // 0..3, or -1 without a city view.
    [[nodiscard]] int Rotation();
    // A city is loaded and cISC4City::IsSaveDisabled() is false.
    [[nodiscard]] bool CanSaveCity();

    // Reads a field of a game object. Returns fallback for a null object.
    template <typename T>
    [[nodiscard]] T ReadField(const void* object, const uint32_t offset, T fallback = T{})
    {
        if (!object) {
            return fallback;
        }
        return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(object) + offset);
    }

    template <typename T>
    [[nodiscard]] T* FieldPtr(void* object, const uint32_t offset)
    {
        return object ? reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset) : nullptr;
    }
}

// Everything a module may use. Filled by the director before any Initialize call. All
// pointers are non-owning and stay valid until the module's Shutdown. Service pointers are
// nullptr when SC4RenderServices does not provide them: check before use. A module pointer is
// never nullptr, but the module may have failed to initialize: it then returns failure from
// its public methods.
struct ModuleContext
{
    // SC4RenderServices
    cIGZImGuiService* imgui = nullptr;
    cIGZDrawService* draw = nullptr;
    cIGZWorldDrawService* worldDraw = nullptr;
    cIGZS3DCameraService* camera = nullptr;

    // Foundation
    Settings* settings = nullptr;
    IdleScheduler* idle = nullptr;
    MessageBus* messages = nullptr;
    RegionIndex* region = nullptr;
    CameraReader* cameraReader = nullptr;
    // The folder of the DLL and the INI file.
    std::filesystem::path pluginDirectory;
    std::filesystem::path settingsPath;

    // Modules, in creation order.
    TileStore* tileStore = nullptr;
    CitySwitcher* switcher = nullptr;
    CameraExtender* cameraExtender = nullptr;
    SnapshotReader* snapshotReader = nullptr;
    ModelLoadMonitor* modelLoadMonitor = nullptr;
    CityCapturer* capturer = nullptr;
    TileCache* tileCache = nullptr;
    NeighborRenderer* neighborRenderer = nullptr;
    NeighborPicker* picker = nullptr;
    RegionBatchRenderer* batch = nullptr;
    HtmlMapExporter* exporter = nullptr;
    MapOverlay* overlay = nullptr;
};

// The lifecycle every module class implements. The director drives it on the game thread.
class Module
{
public:
    Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    virtual ~Module() = default;

    // Short name for logs and the panel.
    [[nodiscard]] virtual const char* Name() const = 0;
    // Called once after PostAppInit when the game version is supported. Return false when
    // the module cannot work, after logging why. The director then still calls Shutdown, but
    // no OnCityInit, OnCityShutdown or DrawPanelSection.
    virtual bool Initialize(const ModuleContext& ctx) = 0;
    // Called once at PostAppShutdown, in reverse creation order. Removes hooks, stops threads.
    virtual void Shutdown() = 0;
    // At kPostCityInit (0x26D31EC1). RegionIndex is already refreshed. The game posts this
    // message: it arrives on a message pump after LoadCity returned, so after a city switch
    // (CitySwitcher) has ended. Do not take "the switch is done" as "OnCityInit has run".
    virtual void OnCityInit() = 0;
    // At kPreCityShutdown (0x26D31EC2), in reverse creation order. The game sends this
    // message: it runs inside CloseCity while the city and its view still exist. It can come
    // without an OnCityInit before it (a city that closed before its kPostCityInit arrived).
    virtual void OnCityShutdown() = 0;
    // Draws this module's part of the panel, inside an ImGui window. Game thread.
    virtual void DrawPanelSection() = 0;
};
