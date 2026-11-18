# Offline tests for the pure parts of the plugin (no game, no DLL).
#
# Every *Tests.cpp file in this folder is built in. Add the pure source files your tests
# need to SC4SR_TEST_SOURCES. Run with: ctest --preset ... or the exe directly.

file(GLOB SC4SR_TEST_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*Tests.cpp")

set(SC4SR_DLL_DIR "${CMAKE_SOURCE_DIR}/src/dll")
set(SC4SR_TEST_SOURCES
    "${SC4SR_DLL_DIR}/view/ViewMath.cpp"
    "${SC4SR_DLL_DIR}/region/RegionLayout.cpp"
    "${SC4SR_DLL_DIR}/store/TileStore.cpp"
    "${SC4SR_DLL_DIR}/store/WebpCodec.cpp"
    "${SC4SR_DLL_DIR}/utils/Logger.cpp"
    "${SC4SR_DLL_DIR}/switching/NeighborDirection.cpp"
    "${SC4SR_DLL_DIR}/camera/CameraBounds.cpp"
)

add_executable(SC4SeamlessRegionTests
    TestMain.cpp
    TestRunner.hpp
    ${SC4SR_TEST_FILES}
    ${SC4SR_TEST_SOURCES}
)

target_include_directories(SC4SeamlessRegionTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SC4SR_DLL_DIR}"
    "${CMAKE_SOURCE_DIR}/vendor/gzcom-dll/gzcom-dll/include"
)

target_compile_definitions(SC4SeamlessRegionTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# Store tests: libwebp, nlohmann-json and the logger (spdlog).
target_link_libraries(SC4SeamlessRegionTests PRIVATE WebP::webp nlohmann_json::nlohmann_json spdlog::spdlog)

if(MSVC)
    set_target_properties(SC4SeamlessRegionTests PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
endif()

add_test(NAME SC4SeamlessRegionTests COMMAND SC4SeamlessRegionTests)
