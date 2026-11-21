"""Time concurrent-downloader against a headless browser downloading the same URL."""

import argparse
import hashlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE_NAME = "concurrent-downloader.exe" if sys.platform == "win32" else "concurrent-downloader"
EXE_CANDIDATES = [
    ROOT / "build" / "vs" / "Release" / EXE_NAME,
    ROOT / "build" / "release" / EXE_NAME,
    ROOT / "build" / EXE_NAME,
    ROOT / "build" / "debug" / EXE_NAME,
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def find_exe(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    for candidate in EXE_CANDIDATES:
        if candidate.exists():
            return candidate
    sys.exit("concurrent-downloader not found; build it or pass --exe")


def run_downloader(exe: Path, url: str, out: Path, connections: int) -> float:
    start = time.perf_counter()
    result = subprocess.run([str(exe), url, str(out), str(connections)], capture_output=True, text=True)
    elapsed = time.perf_counter() - start
    if result.returncode != 0:
        raise RuntimeError(f"concurrent-downloader failed ({connections} connections):\n{result.stderr}")
    return elapsed


def run_browser(page, url: str, out: Path) -> float:
    from playwright.sync_api import Error as PlaywrightError

    start = time.perf_counter()
    with page.expect_download(timeout=0) as info:
        try:
            page.goto(url)
        except PlaywrightError:
            pass  # navigating to a file aborts the page load with "Download is starting"
    download = info.value
    failure = download.failure()  # blocks until the download finishes
    elapsed = time.perf_counter() - start
    if failure:
        raise RuntimeError(f"browser download failed: {failure}")
    download.save_as(out)
    download.delete()
    return elapsed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url", help="file to download; the server must support Range requests")
    parser.add_argument("--connections", type=int, nargs="+", default=[1, 4, 8])
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--exe", help="path to concurrent-downloader (default: first build found)")
    parser.add_argument("--channel", help="installed browser to use instead of Playwright's Chromium, e.g. chrome or msedge")
    parser.add_argument("--no-browser", action="store_true", help="skip the browser baseline")
    args = parser.parse_args()

    exe = find_exe(args.exe)
    print(f"downloader: {exe}")
    work = Path(tempfile.mkdtemp(prefix="cd-bench-"))
    times: dict[str, list[float]] = {}
    hashes: dict[str, str] = {}
    size = 0

    playwright = browser = page = None
    if not args.no_browser:
        from playwright.sync_api import sync_playwright

        playwright = sync_playwright().start()
        browser = playwright.chromium.launch(channel=args.channel)
        page = browser.new_page(accept_downloads=True)

    try:
        for run in range(1, args.runs + 1):
            jobs = [(f"concurrent-downloader x{c}", c) for c in args.connections]
            if page:
                jobs.append((f"browser ({args.channel or 'chromium'})", None))
            # Rotate the order each run so no method always gets the warm or cold cache.
            shift = (run - 1) % len(jobs)
            for name, connections in jobs[shift:] + jobs[:shift]:
                out = work / f"{len(times.get(name, []))}-{connections or 'browser'}.bin"
                if connections is None:
                    elapsed = run_browser(page, args.url, out)
                else:
                    elapsed = run_downloader(exe, args.url, out, connections)
                size = out.stat().st_size
                hashes.setdefault(name, sha256(out))
                times.setdefault(name, []).append(elapsed)
                out.unlink()
                print(f"run {run}/{args.runs}  {name:<28} {elapsed:8.2f} s")
    finally:
        if browser:
            browser.close()
        if playwright:
            playwright.stop()
        shutil.rmtree(work, ignore_errors=True)

    reference = next(iter(hashes.values()))
    print(f"\nURL: {args.url}\nSize: {size / 1e6:.1f} MB, {args.runs} runs, median shown\n")
    print("| Method | Median time (s) | Throughput (MB/s) | Same SHA-256 |")
    print("|---|---:|---:|:---:|")
    for name, samples in times.items():
        median = statistics.median(samples)
        same = "yes" if hashes[name] == reference else "**no**"
        print(f"| {name} | {median:.2f} | {size / 1e6 / median:.1f} | {same} |")


if __name__ == "__main__":
    main()
