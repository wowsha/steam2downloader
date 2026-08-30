# Native Steam2 Extractor

This is the missing materialization stage for `steam2downloader`.

The Python tool downloads the loose Steam2 `.blob` and `.dat` archives. This native helper resolves the blob parent-CRC chain, pairs the required DAT data, decodes Steam2 blocks, and writes the reconstructed manifest to an ordinary directory tree.

## Build

### Windows

Install Visual Studio 2022/2026 with C++ tools and CMake, then from the repository root:

```powershell
cmake -S extractor -B extractor/build -A x64
cmake --build extractor/build --config Release --parallel
```

The executable will be:

```text
extractor/build/Release/steam2extract.exe
```

### Linux

Install a C++23 compiler, CMake, zlib development files, and OpenSSL development files. For Debian/Ubuntu:

```bash
sudo apt install build-essential cmake zlib1g-dev libssl-dev
cmake -S extractor -B extractor/build -DCMAKE_BUILD_TYPE=Release
cmake --build extractor/build --parallel
```

The executable will be:

```text
extractor/build/steam2extract
```

CMake fetches the decoder source and zlib automatically at configure time. The decoder dependency is pinned to a specific `dr3murr/steam2-winfsp` commit for reproducibility.

## Usage

The input directories should contain the downloaded loose archives with their original filenames, for example:

```text
blobs/841_6_800635eb_....blob
blobs/841_5_........_....blob
...
dats/841_6_7a6605a3_....dat
...
```

Extract a depot revision:

```text
steam2extract --blobs /path/to/blobs --dats /path/to/dats --depot 841 --version 6 --output /path/to/extracted
```

If the selected version has multiple blob candidates, specify the CRC from the blob filename:

```text
steam2extract --blobs /path/to/blobs --dats /path/to/dats --depot 841 --version 6 --blobcrc 800635eb --output /path/to/extracted
```

Inspect the manifest without writing files:

```text
steam2extract --blobs /path/to/blobs --dats /path/to/dats --depot 841 --version 6 --list
```

The extractor rejects unsafe archive paths containing `..`, absolute paths, empty components, or Windows drive-style components.

## Why a native decoder

Steam2 is not a collection of self-contained DAT files. The blob metadata describes the revision and its parent CRC, while the content is reconstructed through the DAT/blob revision chain. The decoder therefore has to resolve ancestry before materializing files.

## Next integration step

The GUI downloader still has its original optional `extract.exe` hook. The intended follow-up is to have it discover `steam2extract`, pass the selected depot/version/CRC, and expose an `Extract to directory` action directly beside `Download depot files`.
