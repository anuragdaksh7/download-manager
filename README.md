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

## Usage

```sh
concurrent-downloader <URL> <OUTPUT> [CONNECTIONS]
```

`CONNECTIONS` is 1 to 64 and defaults to 4. The server must report the file size and support HTTP
`Range` requests.

## Tests

`ctest` runs unit tests for chunk splitting and end-to-end downloads against an in-process HTTP server
on `127.0.0.1` (`tests/range_server.hpp`). The server can be told to drop `Accept-Ranges`, ignore
`Range`, fail `HEAD` or hold responses open, which covers the range probe, error cleanup and that
chunks really download in parallel. No network access is needed apart from fetching googletest.

## Benchmark against a browser

`bench/benchmark.py` downloads the same URL with `concurrent-downloader` at several connection counts
and with a headless browser (Playwright), checks every copy has the same SHA-256, and prints a
Markdown table of median time and throughput.

One-time setup (PowerShell):

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\python -m pip install -r bench/requirements.txt
.\.venv\Scripts\python -m playwright install chromium
```

Build a Release binary, then run:

```powershell
cmake --build build/vs --config Release
.\.venv\Scripts\python bench\benchmark.py https://testfile.to/dl/100mb --runs 3 --connections 1 4 8
```

The script uses the first binary it finds in `build/vs/Release`, `build/release`, `build`, then
`build/debug`; pass `--exe` to choose another. `--channel chrome` (or `msedge`) uses your installed
browser instead of Playwright's Chromium, and `--no-browser` skips the browser.

Notes for meaningful numbers:

- Use a file of 100 MB or more; for small files connection setup dominates.
- Some test servers cap connections per IP and answer extra ones with `429 Too Many Requests`
  (e.g. `proof.ovh.net` allows about 3), which fails the download.
- Parallel connections only help when a single connection is slower than your link, e.g. distant or
  per-connection-throttled servers. On a nearby fast server all methods converge.
- Browsers download over a single connection by default (Chrome's `chrome://flags/#enable-parallel-downloading`
  is off), so the browser row is comparable to `concurrent-downloader x1` plus browser overhead.
- Runs are interleaved and the method order rotates each run, but network conditions still vary; use
  `--runs 5` or more before drawing conclusions.
