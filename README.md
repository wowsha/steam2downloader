# steam2tool

Native C++ Steam2 dump indexer, downloader, and extractor. The project no longer requires Python.

## Build

### Linux

Requirements: C++23 compiler, CMake, OpenSSL development files, and `curl`.

```bash
sudo apt install build-essential cmake libssl-dev curl
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Windows

Requirements: Visual Studio 2022/2026 C++ tools, CMake, and `curl.exe` (included with modern Windows).

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

## Usage

Index the German Steam2 mirror's bulk indexes:

```text
steam2tool index --base https://de.steam2.download/ --out export.tsv
```

Download the selected depot/version into `steam2data/dats` and `steam2data/blobs`:

```text
steam2tool download --base https://de.steam2.download/ --index export.tsv --depot 841 --version 6 --dir steam2data
```

Reconstruct and materialize the depot:

```text
steam2tool extract --dir steam2data --depot 841 --version 6 --out extracted/841_6
```

For reset depots, pass the blob CRC when selecting the desired ancestry root:

```text
steam2tool extract --dir steam2data --depot 841 --version 6 --blobcrc 800635eb
```

## Architecture

A single native binary now owns the full pipeline:

`bulk index -> depot/version catalogue -> download -> Steam2 ancestry resolution -> decode -> extracted files`

The Steam2 decoding core is taken from the pinned `dr3murr/steam2-winfsp` implementation. Its source is fetched only as a library component; the project's GUI/FUSE application is not configured or built.
