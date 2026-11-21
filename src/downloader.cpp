#include "downloader/downloader.hpp"
#include <curl/curl.h>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <thread>
#include <atomic>

namespace downloader {

namespace {

struct CurlGlobal {
    CURLcode code;
    CurlGlobal() : code(curl_global_init(CURL_GLOBAL_DEFAULT)) {}
    ~CurlGlobal() {
        if (code == CURLE_OK) {
            curl_global_cleanup();
        }
    }
};

void ensureCurlInitialized() {
    static const CurlGlobal global;

    if (global.code != CURLE_OK) {
        throw std::runtime_error(std::string("curl_global_init failed: ") + curl_easy_strerror(global.code));
    }
}

}  // namespace

Downloader::Downloader(Options options) : options_(options) {
    ensureCurlInitialized();
}

std::size_t Downloader::connections() const noexcept { return options_.connections; }

struct ChunkContext {
    std::ofstream file;
    std::uint64_t received = 0;
    std::string contentRange;
};

static size_t chunkWriteCallback(void* contents, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<ChunkContext*>(userdata);

    const size_t total = size * nmemb;

    ctx->file.write(static_cast<const char*>(contents), static_cast<std::streamsize>(total));

    if (!ctx->file) {
        return 0;
    }

    ctx->received += total;

    return total;
}

static size_t chunkHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* ctx = static_cast<ChunkContext*>(userdata);

    const size_t total = size * nitems;
    std::string header(buffer, total);

    const auto colon = header.find(':');

    if (colon != std::string::npos) {
        std::string key = header.substr(0, colon);

        std::transform(
            key.begin(), key.end(), key.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            }
        );

        if (key == "content-range") {
            ctx->contentRange = header.substr(colon + 1);

            const auto first = ctx->contentRange.find_first_not_of(" \t\r\n");

            if (first != std::string::npos) {
                ctx->contentRange.erase(0, first);
            }
        }
    }

    return total;
}

static bool validateContentRange(const std::string& header, const DownloadChunk& chunk) {
    if (header.rfind("bytes ", 0) != 0) {
        return false;
    }

    try {
        const auto dash = header.find('-', 6);
        const auto slash = header.find('/', dash + 1);

        if (dash == std::string::npos || slash == std::string::npos) {
            return false;
        }

        const auto start = std::stoull(header.substr(6, dash - 6));

        const auto end = std::stoull(header.substr(dash + 1, slash - dash - 1));

        const auto total = std::stoull(header.substr(slash + 1));

        return start == chunk.start && end == chunk.end && total >= end;
    } catch (const std::exception&) {
        return false;
    }
}

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
    if (colon == std::string::npos) {
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

    const auto colon = header.find(':');

    if (colon != std::string::npos) {
        std::string key = header.substr(0, colon);

        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (key == "content-range") {
            *contentRange = header.substr(colon + 1);
        }
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
    auto metadata = fetchMetadata(url);

    if (metadata.size == 0) {
        std::ofstream file(output, std::ios::binary | std::ios::trunc);

        if (!file) {
            throw std::runtime_error("Failed to create output file");
        }

        std::cout << "Download completed: " << output.string() << " (empty file)\n";
        return;
    }

    if (!metadata.supportsRanges) {
        metadata.supportsRanges = probeRangeSupport(url);
    }

    if (!metadata.supportsRanges) {
        throw std::runtime_error("Server does not support range requests");
    }

    std::cout << "File size: "
            << metadata.size << " bytes\n";

    auto chunks = createChunks(metadata.size);

    std::cout << "Connections: "
              << chunks.size() << '\n';

    if (!downloadChunks(url, chunks, output)) {
        throw std::runtime_error("Download failed");
    }

    std::cout << "Download completed: "
              << output.string() << '\n';
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

bool Downloader::downloadChunks(
    const std::string& url,
    const std::vector<DownloadChunk>& chunks,
    const std::filesystem::path& output
) {
    if (chunks.empty()) {
        return true;
    }

    const std::size_t workerCount = (std::min)(
        options_.connections,
        static_cast<std::size_t>(chunks.size())
    );

    if (workerCount == 0) {
        return false;
    }

    std::atomic<std::size_t> nextChunkIndex(0);
    std::atomic<bool> success(true);

    std::vector<std::thread> workers;

    for (std::size_t i = 0; i< workerCount; ++i) {
        workers.emplace_back([&](){
            while (success.load()) {
                const std::size_t index = nextChunkIndex.fetch_add(1);

                if (index >= chunks.size()) {
                    break;
                }
                
                try {
                    if (!downloadChunk(url, chunks[index], output)) {
                        success.store(false);
                        break;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Worker exception: "
                              << e.what() << '\n';

                    success.store(false);
                    break;
                }
            }
        });
    }

    for (auto& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    
    if (!success.load()) {
        for (const auto& chunk : chunks) {
            auto tempPath = output;
            tempPath += ".part." + std::to_string(chunk.start);

            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
        }

        return false;
    }
    std::cout << "All chunks downloaded. Merging...\n";

    std::ofstream destination(
        output,
        std::ios::binary | std::ios::trunc
    );

    if (!destination.is_open()) {
        return false;
    }

    char buffer[64 * 1024];

    for (const auto& chunk : chunks) {
        auto tempPath = output;
        tempPath += ".part." + std::to_string(chunk.start);

        std::ifstream source(tempPath, std::ios::binary);

        if (!source.is_open()) {
            destination.close();
            return false;
        }

        std::uint64_t remaining =
            chunk.end - chunk.start + 1;
        
        while (remaining > 0) {
            const auto toRead = static_cast<std::streamsize>(
                std::min<std::uint64_t>(
                    remaining,
                    sizeof(buffer)
                )
            );

            source.read(buffer, toRead);

            const auto bytesRead = source.gcount();

            if (bytesRead != toRead) {
                source.close();
                destination.close();
                return false;
            }

            destination.write(buffer, bytesRead);

            if (!destination) {
                source.close();
                destination.close();
                return false;
            }

            remaining -= static_cast<std::uint64_t>(bytesRead);
        }

        source.close();

        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }

    destination.close();

    if (!destination) {
        return false;
    }

    std::cout << "File assembled successfully.\n";

    return true;
}

bool Downloader::downloadChunk(
    const std::string& url,
    const DownloadChunk& chunk,
    const std::filesystem::path& output
) {
    CURL* curl = curl_easy_init();

    if (!curl) {
        std::cerr << "Failed to initialize CURL\n";
        return false;
    }

    const auto expectedSize = chunk.end - chunk.start + 1;

    auto tempPath = output;
    tempPath += ".part." + std::to_string(chunk.start);

    ChunkContext ctx;
    ctx.file.open(tempPath, std::ios::binary | std::ios::trunc);

    if (!ctx.file.is_open()) {
        curl_easy_cleanup(curl);
        std::cerr << "Failed to create temporary chunk file\n";
        return false;
    }

    const std::string range =
        std::to_string(chunk.start) + "-" +
        std::to_string(chunk.end);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024);

    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        chunkWriteCallback
    );

    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    curl_easy_setopt(
        curl,
        CURLOPT_HEADERFUNCTION,
        chunkHeaderCallback
    );

    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &ctx);

    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode result = curl_easy_perform(curl);

    long status = 0;

    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &status
    );

    curl_easy_cleanup(curl);

    ctx.file.close();

    // Validate the HTTP response.
    if (result != CURLE_OK ||
        status != 206 ||
        ctx.received != expectedSize ||
        !validateContentRange(ctx.contentRange, chunk)) {

        std::cerr << "Chunk download failed: " << range;

        if (result != CURLE_OK) {
            std::cerr << " | " << curl_easy_strerror(result);
        }

        std::cerr << " | HTTP status: " << status << '\n';

        std::filesystem::remove(tempPath);

        return false;
    }

    std::cout << "Downloaded chunk: " << range
              << " (" << ctx.received << " bytes)\n";

    return true;
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

    if (fileSize < 0) {
        throw std::runtime_error("Server did not report the file size");
    }

    metadata.size = static_cast<std::uint64_t>(fileSize);

    return metadata;
}

}  // namespace downloader
