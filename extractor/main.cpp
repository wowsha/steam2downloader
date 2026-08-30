#include "s2fs/steam2_archive.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path blobs;
    fs::path dats;
    fs::path output;
    std::uint32_t depot{};
    std::uint32_t version{};
    std::optional<std::uint32_t> blob_crc;
    bool list_only{};
};

[[noreturn]] void usage(std::string_view message = {}) {
    if (!message.empty()) {
        std::cerr << "error: " << message << "\n\n";
    }
    std::cerr
        << "Steam2 native extractor\n\n"
        << "usage:\n"
        << "  steam2extract --blobs DIR --dats DIR --depot ID --version N"
           " --output DIR [--blobcrc HEX]\n"
        << "  steam2extract --blobs DIR --dats DIR --depot ID --version N --list"
           " [--blobcrc HEX]\n\n"
        << "The blobs/ and dats/ directories should contain the downloaded"
           " Steam2 archive files.\n";
    throw std::runtime_error("invalid command line");
}

std::uint32_t parse_u32(std::string_view text, std::string_view option, int base) {
    std::uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        usage(std::string(option) + " requires an unsigned integer");
    }
    return value;
}

Options parse_options(int argc, char** argv) {
    Options options;

    auto require_value = [&](int& index, std::string_view option) -> std::string_view {
        if (++index >= argc) {
            usage(std::string(option) + " is missing its value");
        }
        return argv[index];
    };

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--blobs") {
            options.blobs = require_value(index, arg);
        } else if (arg == "--dats") {
            options.dats = require_value(index, arg);
        } else if (arg == "--depot") {
            options.depot = parse_u32(require_value(index, arg), arg, 10);
        } else if (arg == "--version") {
            options.version = parse_u32(require_value(index, arg), arg, 10);
        } else if (arg == "--output") {
            options.output = require_value(index, arg);
        } else if (arg == "--blobcrc") {
            std::string value(require_value(index, arg));
            if (value.starts_with("0x") || value.starts_with("0X")) {
                value.erase(0, 2);
            }
            if (value.size() != 8) {
                usage("--blobcrc must be exactly 8 hexadecimal digits");
            }
            options.blob_crc = parse_u32(value, arg, 16);
        } else if (arg == "--list") {
            options.list_only = true;
        } else if (arg == "--help" || arg == "-h") {
            usage();
        } else {
            usage("unknown option: " + std::string(arg));
        }
    }

    if (options.blobs.empty()) usage("--blobs is required");
    if (options.dats.empty()) usage("--dats is required");
    if (!options.list_only && options.output.empty()) usage("--output is required unless --list is used");
    return options;
}

bool is_bad_component(std::string_view component) {
    return component.empty() || component == "." || component == ".." || component.find(':') != std::string_view::npos;
}

fs::path safe_relative_path(const std::string& archive_path) {
    std::string normalized = archive_path;
    for (char& c : normalized) {
        if (c == '\\') c = '/';
    }

    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    if (normalized.empty()) {
        throw std::runtime_error("archive contains an empty path");
    }

    fs::path result;
    std::size_t start = 0;
    while (start < normalized.size()) {
        const std::size_t slash = normalized.find('/', start);
        const std::size_t end = slash == std::string::npos ? normalized.size() : slash;
        const std::string_view component(normalized.data() + start, end - start);
        if (is_bad_component(component)) {
            throw std::runtime_error("unsafe archive path: " + archive_path);
        }
        result /= fs::path(component);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }

    if (result.empty() || result.is_absolute()) {
        throw std::runtime_error("unsafe archive path: " + archive_path);
    }
    return result;
}

void ensure_directory(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        if (ec) throw fs::filesystem_error("cannot inspect output path", path, ec);
        if (!fs::is_directory(path, ec)) {
            throw std::runtime_error("output path component is not a directory: " + path.string());
        }
        return;
    }
    if (!fs::create_directories(path, ec) && ec) {
        throw fs::filesystem_error("cannot create output directory", path, ec);
    }
}

void materialize_file(const s2fs::DepotEntry& entry, const fs::path& output_root, std::uint64_t& file_count,
                      std::uint64_t& byte_count) {
    if (!entry.file) {
        throw std::runtime_error("manifest entry has no file object: " + entry.path);
    }

    const fs::path relative = safe_relative_path(entry.path);
    const fs::path destination = output_root / relative;
    const fs::path parent = destination.parent_path();
    if (!parent.empty()) ensure_directory(parent);

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create extracted file: " + destination.string());
    }

    constexpr std::size_t chunk_size = 1024 * 1024;
    std::vector<std::byte> buffer(chunk_size);
    std::uint64_t offset = 0;
    const std::uint64_t size = entry.file->size();

    while (offset < size) {
        const std::uint64_t remaining = size - offset;
        const std::size_t requested = static_cast<std::size_t>(
            remaining < buffer.size() ? remaining : buffer.size());
        const std::size_t got = entry.file->read(
            offset, std::span<std::byte>(buffer.data(), requested));
        if (got == 0 || got > requested) {
            throw std::runtime_error("invalid Steam2 read while extracting: " + entry.path);
        }
        output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(got));
        if (!output) {
            throw std::runtime_error("write failed while extracting: " + destination.string());
        }
        offset += got;
    }

    ++file_count;
    byte_count += size;
    std::cout << "  " << entry.path << " (" << size << " bytes)\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);

        if (!fs::is_directory(options.blobs)) {
            throw std::runtime_error("blobs path is not a directory: " + options.blobs.string());
        }
        if (!fs::is_directory(options.dats)) {
            throw std::runtime_error("dats path is not a directory: " + options.dats.string());
        }

        s2fs::DepotSpec spec;
        spec.id = options.depot;
        spec.version = options.version;
        spec.blob_crc = options.blob_crc;
        spec.blob_directory = options.blobs;
        spec.dat_directory = options.dats;

        std::cout << "Loading Steam2 depot " << spec.id << " version " << spec.version;
        if (spec.blob_crc) {
            std::cout << " (blob CRC " << std::hex << *spec.blob_crc << std::dec << ")";
        }
        std::cout << "...\n";

        const s2fs::Steam2Depot depot = s2fs::Steam2Depot::load(spec);
        std::cout << "Resolved " << depot.entries().size() << " manifest entries.\n";

        if (options.list_only) {
            for (const s2fs::DepotEntry& entry : depot.entries()) {
                const std::uint64_t size = entry.file ? entry.file->size() : 0;
                std::cout << size << "\t" << entry.path << "\n";
            }
            return 0;
        }

        ensure_directory(options.output);

        std::uint64_t file_count = 0;
        std::uint64_t byte_count = 0;
        for (const s2fs::DepotEntry& entry : depot.entries()) {
            materialize_file(entry, options.output, file_count, byte_count);
        }

        std::cout << "Extracted " << file_count << " files (" << byte_count << " bytes) to "
                  << options.output << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
