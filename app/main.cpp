#include "downloader/downloader.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <url> <output>\n";
        return 1;
    }

    downloader::Downloader dl;
    dl.download(argv[1], argv[2]);
    return 0;
}
