#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <cstdint>
#include <vector>

namespace downloader {

struct Options {
    std::size_t connections = 4;
};

struct FileMetadata {
    std::uint64_t size = 0;
    bool supportsRanges = false;
    std::string etag;
    std::string lastModified;
};

struct DownloadChunk {
    std::uint64_t start;
    std::uint64_t end;
};

class Downloader {
public:
    explicit Downloader(Options options = {});

    [[nodiscard]] std::size_t connections() const noexcept;

    void download(const std::string& url, const std::filesystem::path& output);

    FileMetadata fetchMetadata(const std::string& url);

    std::vector<DownloadChunk> createChunks(std::uint64_t fileSize) const;

    bool downloadChunk(const std::string& url, const DownloadChunk& chunk, const std::filesystem::path& output);

private:
    Options options_;

    void testCurl();
};

}  // namespace downloader
