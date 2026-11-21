#include "downloader/downloader.hpp"

#include <gtest/gtest.h>

TEST(Downloader, UsesDefaultConnections) {
    downloader::Downloader dl;
    EXPECT_EQ(dl.connections(), 4u);
}

TEST(Downloader, RespectsConfiguredConnections) {
    downloader::Downloader dl({.connections = 8});
    EXPECT_EQ(dl.connections(), 8u);
}
