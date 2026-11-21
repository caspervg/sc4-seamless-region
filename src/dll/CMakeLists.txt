if(NOT WIN32)
    message(STATUS "SC4SeamlessRegion is only built on Win32. Skipping the dll target.")
    return()
endif()

set(SC4_RENDER_SERVICES_ROOT "${CMAKE_SOURCE_DIR}/vendor/sc4-render-services" CACHE PATH "Path to sc4-render-services")
set(SC4_RENDER_SERVICES_PUBLIC_DIR "${SC4_RENDER_SERVICES_ROOT}/src")

set(GZCOM_DIR "${CMAKE_SOURCE_DIR}/vendor/gzcom-dll/gzcom-dll")
set(SC4_DLL_UTILITIES_DIR "${CMAKE_SOURCE_DIR}/vendor/sc4-dll-utilities/sc4-dll-utilities")

file(GLOB_RECURSE GZCOM_HEADERS CONFIGURE_DEPENDS
    "${GZCOM_DIR}/include/*.h"
    "${GZCOM_DIR}/include/*.hpp"
)
file(GLOB_RECURSE GZCOM_SOURCES CONFIGURE_DEPENDS
    "${GZCOM_DIR}/src/*.cpp"
)
list(REMOVE_ITEM GZCOM_SOURCES
    "${GZCOM_DIR}/src/main.cpp"
    "${GZCOM_DIR}/src/cGZExampleDllDirector.cpp"
    "${GZCOM_DIR}/src/cGZExtraCheatsDllDirector.cpp"
    "${GZCOM_DIR}/src/cGZExtraExtraCheatsDllDirector.cpp"
    "${GZCOM_DIR}/src/cGZCityHallUpgradeDllDirector.cpp"
)

add_library(gzcom2 STATIC ${GZCOM_SOURCES} ${GZCOM_HEADERS})
target_include_directories(gzcom2 PUBLIC "${GZCOM_DIR}/include")

# ImGui is compiled into SC4RenderServices.dll and exported from it (ImGui service API 5):
# plugins must call that ImGui. Building SC4RenderServices from the submodule gives its import
# library. That needs the nested submodules imgui, Clipper2 and earcut.
set(SC4RS_NESTED_SUBMODULES_READY OFF)
if(EXISTS "${SC4_RENDER_SERVICES_ROOT}/vendor/imgui/imgui.h"
   AND EXISTS "${SC4_RENDER_SERVICES_ROOT}/vendor/Clipper2/CPP/Clipper2Lib/include/clipper2/clipper.h"
   AND EXISTS "${SC4_RENDER_SERVICES_ROOT}/vendor/earcut/include/mapbox/earcut.hpp")
    set(SC4RS_NESTED_SUBMODULES_READY ON)
    # Reuses gzcom2 and spdlog::spdlog. EXCLUDE_FROM_ALL keeps its other targets out.
    add_subdirectory("${SC4_RENDER_SERVICES_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/sc4-render-services" EXCLUDE_FROM_ALL)
else()
    message(WARNING
        "sc4-render-services is missing its nested submodules (imgui, Clipper2, earcut). "
        "Building with the old d3d7imgui headers and the Seamless Region panel turned off. "
        "Run 'git submodule update --init --recursive' for a working panel.")
    include("${CMAKE_SOURCE_DIR}/cmake/LegacyImGui.cmake")
endif()

file(GLOB_RECURSE SC4_DLL_UTILITIES_HEADERS CONFIGURE_DEPENDS
    "${SC4_DLL_UTILITIES_DIR}/include/*.h"
    "${SC4_DLL_UTILITIES_DIR}/include/*.hpp"
)
file(GLOB_RECURSE SC4_DLL_UTILITIES_SOURCES CONFIGURE_DEPENDS
    "${SC4_DLL_UTILITIES_DIR}/src/*.cpp"
)
list(REMOVE_ITEM SC4_DLL_UTILITIES_SOURCES
    "${SC4_DLL_UTILITIES_DIR}/src/Logger.cpp"
)

find_path(MINI_INCLUDE_DIR NAMES mini/ini.h)
if(NOT MINI_INCLUDE_DIR)
    message(FATAL_ERROR "mini/ini.h was not found. Ensure the pulzed-mini vcpkg dependency is installed.")
endif()

file(GLOB_RECURSE PROJECT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
)

string(REPLACE "." "," SC4_RESOURCE_FILE_VERSION "${PROJECT_VERSION}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)

add_library(${PROJECT_NAME} SHARED
    ${PROJECT_SOURCES}
    ${SC4_DLL_UTILITIES_HEADERS}
    ${SC4_DLL_UTILITIES_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/SC4SeamlessRegion.def"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    SC4SR_DLL_NAME="${PROJECT_NAME}"
    SC4SR_DLL_VERSION="${PROJECT_VERSION}"
    SC4SR_DLL_VERSION_LABEL="${SC4SR_APP_VERSION}"
)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${MINI_INCLUDE_DIR}"
    "${SC4_DLL_UTILITIES_DIR}/include"
)

target_include_directories(${PROJECT_NAME} PRIVATE "${SC4_RENDER_SERVICES_PUBLIC_DIR}")

target_link_libraries(${PROJECT_NAME} PRIVATE
    gzcom2
    spdlog::spdlog
    WIL::WIL
    WebP::webp
    WebP::webpdemux
    minhook::minhook
    nlohmann_json::nlohmann_json
    kernel32
    user32
    Version
    ddraw
    dxguid
)

if(SC4RS_NESTED_SUBMODULES_READY)
    # Links the ImGui exported by SC4RenderServices.dll, delay-loaded so load order does not matter.
    sc4rs_use_imgui(${PROJECT_NAME})
else()
    # Compiles against the old ImGui only. The DLL never calls it: the director drops the ImGui
    # service and registers no panel. Delay loading keeps imgui.dll from being required.
    target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_DLL_IMPORT SC4SR_IMGUI_PANEL_DISABLED)
    target_link_libraries(${PROJECT_NAME} PRIVATE SC4RenderServices::imgui delayimp)
    target_link_options(${PROJECT_NAME} PRIVATE "/DELAYLOAD:imgui.dll")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    OUTPUT_NAME "SC4SeamlessRegion"
    SUFFIX ".dll"
    PREFIX ""
)

if(MSVC)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()

if(WIN32 AND SC4_ENABLE_PLUGIN_DEPLOYMENT)
    if(SC4_PLUGINS_DIR)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SC4_PLUGINS_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${PROJECT_NAME}>
                "${SC4_PLUGINS_DIR}/"
            COMMAND ${CMAKE_COMMAND}
                -DSRC=${CMAKE_SOURCE_DIR}/dist/SC4SeamlessRegion.ini
                -DDST=${SC4_PLUGINS_DIR}/SC4SeamlessRegion.ini
                -P ${CMAKE_SOURCE_DIR}/cmake/CopyIfNotExists.cmake
            COMMENT "Deploying SC4SeamlessRegion.dll to the SimCity 4 Plugins folder"
        )
        message(STATUS "Plugin deployment enabled: ${SC4_PLUGINS_DIR}")
    else()
        message(WARNING "SC4_PLUGINS_DIR is empty. Automatic Plugins deployment is disabled.")
    endif()
endif()
