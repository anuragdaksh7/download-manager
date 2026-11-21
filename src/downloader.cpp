#include "downloader/downloader.hpp"
#include <curl/curl.h>
#include <iostream>

namespace downloader {

Downloader::Downloader(Options options) : options_(options) {}

std::size_t Downloader::connections() const noexcept { return options_.connections; }

void Downloader::download(const std::string& url, const std::filesystem::path& output) {
    std::cout << "TODO: download " << url << " to " << output.string() << " over "
              << options_.connections << " connections\n";
    
    testCurl();
    
}

void Downloader::testCurl() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    CURL* curl = curl_easy_init();

    if (curl) {
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
}

}  // namespace downloader
