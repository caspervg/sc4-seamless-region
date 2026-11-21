# SC4LoadCache: faster city loads (key lists by type and the Type 21 scan, kept for the session).
if(NOT WIN32)
    return()
endif()
add_library(SC4LoadCache SHARED LoadCache.cpp SC4LoadCache.def)
target_compile_definitions(SC4LoadCache PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SC4LoadCache PRIVATE gzcom2 minhook::minhook version)
