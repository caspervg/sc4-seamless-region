# SC4LoadProfiler: a test DLL that times city loads (not part of the mod, not shipped).
if(NOT WIN32)
    return()
endif()
add_library(SC4LoadProfiler SHARED LoadProfiler.cpp SC4LoadProfiler.def)
target_compile_definitions(SC4LoadProfiler PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SC4LoadProfiler PRIVATE gzcom2 minhook::minhook winmm)
