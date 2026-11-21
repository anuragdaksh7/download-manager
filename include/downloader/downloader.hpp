#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace downloader {

struct Options {
    std::size_t connections = 4;
};

class Downloader {
public:
    explicit Downloader(Options options = {});

    [[nodiscard]] std::size_t connections() const noexcept;

    void download(const std::string& url, const std::filesystem::path& output);

private:
    Options options_;

    void testCurl();
};

}  // namespace downloader
