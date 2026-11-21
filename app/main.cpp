#include "downloader/downloader.hpp"
#include <curl/curl.h>

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: concurrent-downloader <URL> <OUTPUT> [CONNECTIONS]\n";
        return 1;
    }

    downloader::Options options;

    if (argc == 4) {
        try {
            options.connections = static_cast<std::size_t>(std::stoull(argv[3]));
        } catch (const std::exception&) {
            options.connections = 0;
        }

        if (options.connections == 0) {
            std::cerr << "CONNECTIONS must be a positive integer\n";
            return 1;
        }
    }

    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);

    if (result != CURLE_OK) {
        std::cerr << "CURL initialization failed\n";
        return 1;
    }

    int exitCode = 0;

    try
    {
        downloader::Downloader dl(options);
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
