#include "downloader/downloader.hpp"
#include <curl/curl.h>

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: concurrent-downloader <URL> <OUTPUT>\n";
        return 1;
    }

    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);

    if (result != CURLE_OK) {
        std::cerr << "CURL initialization failed\n";
        return 1;
    }

    int exitCode = 0;

    try
    {
        downloader::Downloader dl;
        dl.download(argv[1], argv[2]);
    }
    catch(const std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << '\n';
        exitCode = 1;
    }

    curl_global_cleanup();
    
    return exitCode;
}
