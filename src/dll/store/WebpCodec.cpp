#include "store/WebpCodec.hpp"

#include <atomic>

#include "utils/Logger.h"

namespace
{
    std::atomic_flag sLogged = ATOMIC_FLAG_INIT;

    void NotImplemented(const char* what)
    {
        if (!sLogged.test_and_set()) {
            LOG_WARN("WebpCodec: {} is not implemented yet", what);
        }
    }
}

namespace WebpCodec
{
    bool Encode(const uint8_t*, int, int, int, std::vector<uint8_t>& out)
    {
        NotImplemented("Encode");
        out.clear();
        return false;
    }

    bool Decode(std::span<const uint8_t>, std::vector<uint8_t>& rgba, int& w, int& h)
    {
        NotImplemented("Decode");
        rgba.clear();
        w = 0;
        h = 0;
        return false;
    }
}
