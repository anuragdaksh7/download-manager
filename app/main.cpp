#include "downloader/downloader.hpp"

#include <charconv>
#include <cstring>
#include <iostream>

namespace {

constexpr std::size_t kMaxConnections = 64;

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: concurrent-downloader <URL> <OUTPUT> [CONNECTIONS]\n";
        return 1;
    }

    downloader::Options options;

    if (argc == 4) {
        const char* text = argv[3];
        const char* end = text + std::strlen(text);
        std::size_t connections = 0;
        const auto [ptr, ec] = std::from_chars(text, end, connections);

        if (ec != std::errc() || ptr != end || connections == 0 || connections > kMaxConnections) {
            std::cerr << "CONNECTIONS must be an integer from 1 to " << kMaxConnections << '\n';
            return 1;
        }

        options.connections = connections;
    }

    try {
        downloader::Downloader dl(options);
        dl.download(argv[1], argv[2]);
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
