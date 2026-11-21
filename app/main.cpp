#include "downloader/downloader.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <url> <output>\n";
        return 1;
    }

    try
    {
        downloader::Downloader dl;
        dl.download(argv[1], argv[2]);
    }
    catch(const std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << '\n';
        return 1;
    }
    
    return 0;
}
