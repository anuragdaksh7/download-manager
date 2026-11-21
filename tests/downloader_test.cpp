#include "downloader/downloader.hpp"
#include "range_server.hpp"

#include <curl/curl.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using downloader::Downloader;
using downloader::DownloadChunk;
using test_support::RangeServer;
using test_support::ServerBehaviour;

namespace {

class CurlEnvironment : public ::testing::Environment {
public:
    void SetUp() override { curl_global_init(CURL_GLOBAL_DEFAULT); }
    void TearDown() override { curl_global_cleanup(); }
};

[[maybe_unused]] auto* const curlEnvironment = ::testing::AddGlobalTestEnvironment(new CurlEnvironment);

std::string randomBytes(std::size_t size) {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> byte(0, 255);
    std::string data(size, '\0');
    for (auto& c : data) {
        c = static_cast<char>(byte(rng));
    }
    return data;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class DownloadTest : public ::testing::Test {
protected:
    fs::path dir;
    fs::path output;

    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::temp_directory_path() / ("concurrent-downloader-" + std::string(info->name()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        output = dir / "out.bin";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    [[nodiscard]] std::size_t filesInDir() const {
        return static_cast<std::size_t>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
    }
};

}  // namespace

TEST(Options, DefaultsToFourConnections) {
    EXPECT_EQ(Downloader().connections(), 4u);
}

TEST(Options, RespectsConfiguredConnections) {
    EXPECT_EQ(Downloader({.connections = 8}).connections(), 8u);
}

TEST(CreateChunks, EmptyFileHasNoChunks) {
    EXPECT_TRUE(Downloader().createChunks(0).empty());
}

TEST(CreateChunks, ZeroConnectionsHasNoChunks) {
    EXPECT_TRUE(Downloader({.connections = 0}).createChunks(100).empty());
}

TEST(CreateChunks, SpreadsRemainderOverFirstChunks) {
    const auto chunks = Downloader({.connections = 3}).createChunks(10);
    ASSERT_EQ(chunks.size(), 3u);
    EXPECT_EQ(chunks[0].start, 0u);
    EXPECT_EQ(chunks[0].end, 3u);
    EXPECT_EQ(chunks[1].start, 4u);
    EXPECT_EQ(chunks[1].end, 6u);
    EXPECT_EQ(chunks[2].start, 7u);
    EXPECT_EQ(chunks[2].end, 9u);
}

TEST(CreateChunks, NeverMoreChunksThanBytes) {
    const auto chunks = Downloader({.connections = 8}).createChunks(3);
    ASSERT_EQ(chunks.size(), 3u);
    for (std::uint64_t i = 0; i < 3; ++i) {
        EXPECT_EQ(chunks[i].start, i);
        EXPECT_EQ(chunks[i].end, i);
    }
}

TEST(CreateChunks, CoversWholeFileContiguously) {
    for (std::size_t connections : {1u, 2u, 3u, 7u, 16u}) {
        for (std::uint64_t size : {1ull, 2ull, 1000ull, 1'048'577ull}) {
            const auto chunks = Downloader({.connections = connections}).createChunks(size);
            ASSERT_FALSE(chunks.empty());
            EXPECT_EQ(chunks.front().start, 0u);
            EXPECT_EQ(chunks.back().end, size - 1);
            for (std::size_t i = 1; i < chunks.size(); ++i) {
                EXPECT_EQ(chunks[i].start, chunks[i - 1].end + 1) << connections << " x " << size;
            }
        }
    }
}

TEST_F(DownloadTest, FetchMetadataReadsHeaders) {
    RangeServer server(randomBytes(1234));
    const auto metadata = Downloader().fetchMetadata(server.url());
    EXPECT_EQ(metadata.size, 1234u);
    EXPECT_TRUE(metadata.supportsRanges);
    EXPECT_EQ(metadata.etag, "\"test-etag\"");
}

TEST_F(DownloadTest, FetchMetadataThrowsOnHttpError) {
    RangeServer server("data", {.headStatus = 404});
    EXPECT_THROW(Downloader().fetchMetadata(server.url()), std::runtime_error);
}

TEST_F(DownloadTest, DownloadsIdenticalFile) {
    const auto body = randomBytes(1 << 20);
    RangeServer server(body);

    Downloader({.connections = 4}).download(server.url(), output);

    EXPECT_EQ(readFile(output), body);
    EXPECT_EQ(server.rangeRequests(), 4);
    EXPECT_EQ(filesInDir(), 1u) << "temporary .part files were left behind";
}

TEST_F(DownloadTest, DownloadsWithSingleConnection) {
    const auto body = randomBytes(100'000);
    RangeServer server(body);

    Downloader({.connections = 1}).download(server.url(), output);

    EXPECT_EQ(readFile(output), body);
    EXPECT_EQ(server.rangeRequests(), 1);
}

TEST_F(DownloadTest, DownloadsFileSmallerThanConnectionCount) {
    RangeServer server("abc");

    Downloader({.connections = 8}).download(server.url(), output);

    EXPECT_EQ(readFile(output), "abc");
    EXPECT_EQ(server.rangeRequests(), 3);
}

TEST_F(DownloadTest, FetchesChunksInParallel) {
    RangeServer server(randomBytes(64 * 1024), {.rangeDelay = std::chrono::milliseconds(300)});

    Downloader({.connections = 4}).download(server.url(), output);

    EXPECT_EQ(server.maxConcurrentRanges(), 4);
}

TEST_F(DownloadTest, ProbesRangeSupportWhenNotAdvertised) {
    const auto body = randomBytes(10'000);
    RangeServer server(body, {.advertiseRanges = false});

    Downloader().download(server.url(), output);

    EXPECT_EQ(readFile(output), body);
}

TEST_F(DownloadTest, RejectsServerWithoutRangeSupport) {
    RangeServer server(randomBytes(10'000), {.advertiseRanges = false, .honourRanges = false});

    EXPECT_THROW(Downloader().download(server.url(), output), std::runtime_error);
    EXPECT_EQ(filesInDir(), 0u);
}

TEST_F(DownloadTest, FailsAndCleansUpWhenServerIgnoresRanges) {
    RangeServer server(randomBytes(10'000), {.advertiseRanges = true, .honourRanges = false});

    EXPECT_THROW(Downloader().download(server.url(), output), std::runtime_error);
    EXPECT_EQ(filesInDir(), 0u);
}
