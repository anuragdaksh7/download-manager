include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(downloader_tests downloader_test.cpp)
target_link_libraries(downloader_tests PRIVATE downloader project_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(downloader_tests)
