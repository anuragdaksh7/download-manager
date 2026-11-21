# concurrent-downloader

Downloads a single file over several parallel TCP connections, each fetching a separate byte range
(HTTP `Range` requests) and writing it into its offset in the output file.

## Build

Requires CMake 3.25+, Ninja, a C++23 compiler and libcurl.

### With vcpkg and MSVC

The `x64-windows` triplet is built with 64-bit MSVC, so build from a 64-bit VS developer shell and
use `cl`. Otherwise CMake picks MSYS2's GCC and curl from `PATH`. The Start-menu "Developer PowerShell
for VS 2022" defaults to 32-bit (`Hostx86/x86/cl.exe`, and CMake then rejects the 64-bit curl), so
switch it first:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
```

Install curl once:

```powershell
X:/code/tools/vcpkg/vcpkg install curl:x64-windows
```

Configure, build, test and run:

```powershell
cmake --fresh -B build -S . -G Ninja -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_TOOLCHAIN_FILE=X:/code/tools/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build
ctest --test-dir build
.\build\concurrent-downloader.exe https://example.com/file.iso file.iso
```

`--fresh` is needed only when `build` was previously configured with another compiler or toolchain;
CMake ignores `CMAKE_TOOLCHAIN_FILE` on an existing cache.

### With presets (MSYS2 UCRT64 GCC and curl)

`C:\msys64\ucrt64\bin` must come before Git's `mingw64\bin` on `PATH`, otherwise GCC fails silently.

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/debug/concurrent-downloader https://example.com/file.iso file.iso
```

Use `--preset release` for an optimized build.
