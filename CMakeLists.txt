
cmake_minimum_required(VERSION 3.25)

project(concurrent_downloader VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DOWNLOADER_BUILD_TESTS "Build unit tests" ON)

find_package(CURL REQUIRED)

add_library(project_warnings INTERFACE)

if(MSVC)
  target_compile_options(project_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(project_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
  )
endif()

add_library(downloader
  src/downloader.cpp
)

target_include_directories(downloader
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(downloader
  PRIVATE project_warnings
  PUBLIC CURL::libcurl
)

add_executable(concurrent-downloader app/main.cpp)

target_link_libraries(concurrent-downloader
  PRIVATE downloader project_warnings
)

if(DOWNLOADER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
