# SC4RegionMap.exe: the region map export outside the game.

set(SC4SR_DLL_DIR "${CMAKE_SOURCE_DIR}/src/dll")

add_executable(SC4RegionMap
    SC4RegionMap.cpp
    "${SC4SR_DLL_DIR}/export/MapExport.cpp"
    "${SC4SR_DLL_DIR}/store/TileStore.cpp"
    "${SC4SR_DLL_DIR}/store/WebpCodec.cpp"
    "${SC4SR_DLL_DIR}/region/RegionLayout.cpp"
    "${SC4SR_DLL_DIR}/view/ViewMath.cpp"
    "${SC4SR_DLL_DIR}/utils/Logger.cpp"
)

target_include_directories(SC4RegionMap PRIVATE
    "${SC4SR_DLL_DIR}"
    "${CMAKE_SOURCE_DIR}/vendor/gzcom-dll/gzcom-dll/include"
)

target_compile_definitions(SC4RegionMap PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(SC4RegionMap PRIVATE WebP::webp nlohmann_json::nlohmann_json spdlog::spdlog shell32)

if(MSVC)
    set_target_properties(SC4RegionMap PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()
