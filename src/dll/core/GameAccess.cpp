#include "cIGZDate.h"
#include "cISC4Simulator.h"
#include "core/GameAccess.hpp"

#include "GZServPtrs.h"
#include "SC4VersionDetection.h"
#include "cIGZWin.h"
#include "cISC43DRender.h"
#include "cISC4App.h"
#include "cISC4City.h"
#include "cISC4View3DWin.h"
#include "core/GameAddresses.hpp"
#include "utils/Logger.h"

namespace
{
    constexpr uint32_t kGZWin_WinSC4App = 0x6104489Au;
    constexpr uint32_t kGZWin_SC4View3DWin = 0x9A47B417u;

    cISC4App* sApp = nullptr;
    cISC4View3DWin* sView3D = nullptr;
    // Between PostCityInit and PreCityShutdown. Keeps a stale view pointer from leaking out.
    bool sCityOpen = false;
    int sCachedVersion = -1;
}

namespace GameAccess
{
    uint16_t GameVersion()
    {
        if (sCachedVersion < 0) {
            sCachedVersion = SC4VersionDetection::GetGameVersion();
        }
        return static_cast<uint16_t>(sCachedVersion);
    }

    bool IsSupportedGame() { return GameVersion() == GameAddresses::kSupportedGameVersion; }

    void Initialize()
    {
        if (sApp) {
            return;
        }
        cISC4AppPtr app;
        sApp = app;
        if (sApp) {
            // The app lives as long as the process. Keep one reference until Shutdown.
            sApp->AddRef();
        }
        else {
            LOG_ERROR("GameAccess: cISC4App is not available");
        }
    }

    void Shutdown()
    {
        OnCityShutdown();
        if (sApp) {
            sApp->Release();
            sApp = nullptr;
        }
    }

    void OnCityInit()
    {
        sCityOpen = true;
        sView3D = nullptr;
    }

    void OnCityShutdown()
    {
        sCityOpen = false;
        sView3D = nullptr;
    }

    cISC4App* App() { return sApp; }

    cISC4City* City() { return sApp ? sApp->GetCity() : nullptr; }

    int CityMonth()
    {
        cISC4City* city = City();
        cISC4Simulator* simulator = city ? city->GetSimulator() : nullptr;
        cIGZDate* date = simulator ? simulator->GetSimDate() : nullptr;
        const uint32_t month = date ? date->Month() : 0;
        return month >= 1 && month <= 12 ? static_cast<int>(month) : 0;
    }

    cISC4Region* Region() { return sApp ? sApp->GetRegion() : nullptr; }

    cISC4RegionalCity* RegionalCity() { return sApp ? sApp->GetRegionalCity() : nullptr; }

    cISC4View3DWin* View3D()
    {
        if (!sCityOpen) {
            return nullptr;
        }
        if (sView3D) {
            return sView3D;
        }
        if (!sApp || !City()) {
            return nullptr;
        }
        cIGZWin* mainWindow = sApp->GetMainWindow();
        cIGZWin* appWindow = mainWindow ? mainWindow->GetChildWindowFromID(kGZWin_WinSC4App) : nullptr;
        cISC4View3DWin* view = nullptr;
        if (appWindow &&
            appWindow->GetChildAs(kGZWin_SC4View3DWin, kGZIID_cISC4View3DWin, reinterpret_cast<void**>(&view)) &&
            view) {
            // The window owns itself while the city is open. Keep a plain pointer, dropped
            // at OnCityShutdown.
            view->Release();
            sView3D = view;
        }
        return sView3D;
    }

    cISC43DRender* Renderer()
    {
        cISC4View3DWin* view = View3D();
        return view ? view->GetRenderer() : nullptr;
    }

    cSC4CameraControl* CameraControl()
    {
        // cISC43DRender slot +0x40 is cSC43DRender::GetCameraControl (0x007c9fd0).
        cISC43DRender* renderer = Renderer();
        return renderer ? renderer->GetCameraControl() : nullptr;
    }

    cIGZGDriver* GDriver()
    {
        auto* driver = *reinterpret_cast<cIGZGDriver**>(GameAddresses::Renderer::kGDriverPointer);
        if (!driver) {
            driver = *reinterpret_cast<cIGZGDriver**>(GameAddresses::Renderer::kGDriverPointerGlobal);
        }
        return driver;
    }

    int Zoom()
    {
        return ReadField<int32_t>(CameraControl(), GameAddresses::CameraControl::kZoom, -1);
    }

    int Rotation()
    {
        return ReadField<int32_t>(CameraControl(), GameAddresses::CameraControl::kRotation, -1);
    }

    bool CanSaveCity()
    {
        cISC4City* city = City();
        return city && !city->IsSaveDisabled();
    }
}
