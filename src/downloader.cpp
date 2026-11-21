#include "downloader/downloader.hpp"
#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace downloader {

Downloader::Downloader(Options options) : options_(options) {}

std::size_t Downloader::connections() const noexcept { return options_.connections; }

static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userdata) {
    auto* file = static_cast<std::ofstream*>(userdata);

    const size_t total = size * nmemb;

    file->write(static_cast<const char*>(contents), static_cast<std::streamsize>(total));
    
    return *file ? total : 0;
}

static size_t headerCallback(char*buffer, size_t size, size_t nitems, void* userdata) {
    auto* metadata = static_cast<FileMetadata*>(userdata);

    size_t total = size * nitems;
    std::string header(buffer, total);

    auto colon = header.find(':');
    if (colon != std::string::npos) {
        return total;
    }

    std::string key = header.substr(0, colon);
    std::string value = header.substr(colon + 1);

    auto trim = [](std::string& str) {
        auto start = str.find_first_not_of(" \t\r\n");

        if (start == std::string::npos) {
            str.clear();
            return;
        }

        auto end = str.find_last_not_of(" \t\r\n");
        str = str.substr(start, end - start + 1);
    };

    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    trim(value);

    if (key == "accept-ranges") {
        metadata->supportsRanges = (value == "bytes");
    } else if (key == "etag") {
        metadata->etag = value;
    } else if (key == "last-modified") {
        metadata->lastModified = value;
    }

    return total;
}

static size_t discardCallback(
    void* contents,
    size_t size,
    size_t nmemb,
    void* userdata
) {
    return size * nmemb;
}

static size_t contentRangeCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;

    auto* contentRange = static_cast<std::string*>(userdata);

    std::string header(buffer, total);

    if (header.size() >= 14 &&
        header.substr(0, 14) == "Content-Range:") {
        *contentRange = header.substr(14);

    }

    return total;
}

static bool probeRangeSupport(const std::string& url) {
    CURL* curl = curl_easy_init();

    if (!curl) {
        return false;
    }

    std::string contentRange;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardCallback);

    curl_easy_setopt(
        curl,
        CURLOPT_HEADERFUNCTION,
        contentRangeCallback
    );

    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &contentRange);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode result = curl_easy_perform(curl);

    long status = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &status
    );

    curl_easy_cleanup(curl);

    return result == CURLE_OK &&
           status == 206 &&
           !contentRange.empty();
}

void Downloader::download(const std::string& url, const std::filesystem::path& output) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    auto metadata = fetchMetadata(url);

    if (!metadata.supportsRanges) {
        metadata.supportsRanges = probeRangeSupport(url);
    }

    if (!metadata.supportsRanges) {
        throw std::runtime_error("Server does not support range requests");
    }

    std::cout << "File size: "
              << metadata.size << " bytes\n";

    std::cout << "Range support: "
              << std::boolalpha
              << metadata.supportsRanges << '\n';

    auto chunks = createChunks(metadata.size);

    for (std::size_t i = 0; i < chunks.size(); ++i) {
        std::cout
            << "Chunk " << i + 1
            << ": " << chunks[i].start
            << "-" << chunks[i].end
            << " (" << chunks[i].end - chunks[i].start + 1
            << " bytes)\n";
    }

    curl_global_cleanup();
}

std::vector<DownloadChunk> Downloader::createChunks(std::uint64_t fileSize) const {
    std::vector<DownloadChunk> chunks;

    if (fileSize == 0 || options_.connections == 0) {
        return chunks;
    }

    const std::uint64_t connections = (std::min)(static_cast<std::uint64_t>(options_.connections), fileSize);

    const std::uint64_t chunkSize = fileSize / connections;
    const std::uint64_t remainder = fileSize % connections;

    std::uint64_t start = 0;

    for (std::uint64_t i = 0; i < connections; ++i) {
        const std::uint64_t currentChunkSize =
            chunkSize + (i < remainder ? 1 : 0);

            const std::uint64_t end = start + currentChunkSize - 1;

            chunks.push_back({start, end});

            start = end + 1;
    }

    return chunks;
}

void Downloader::testCurl() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    CURL* curl = curl_easy_init();

    if (curl) {
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
}

FileMetadata Downloader::fetchMetadata(const std::string& url) {
    FileMetadata metadata;

    CURL* curl = curl_easy_init();

    if (!curl) {
        throw std::runtime_error("Failed to initialize curl");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &metadata);

    CURLcode result = curl_easy_perform(curl);

    if (result != CURLE_OK) {
        std::string error = curl_easy_strerror(result);
        curl_easy_cleanup(curl);
        throw std::runtime_error(error);
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_off_t fileSize = -1;

    curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &fileSize);

    curl_easy_cleanup(curl);

    if (status < 200 || status >= 300) {
        throw std::runtime_error("HEAD request failed with HTTP status " + std::to_string(status));
    }

    if (fileSize >= 0) {
        metadata.size = static_cast<std::uint64_t>(fileSize);
    }

    return metadata;
}

}  // namespace downloader
