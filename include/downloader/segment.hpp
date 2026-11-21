#pragma once

#include <cstdint>

namespace downloader {

struct Segment {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

}  // namespace downloader
