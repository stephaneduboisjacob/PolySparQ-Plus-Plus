#include "config/config.hpp"
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using namespace vp;
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("polysparq-config-data-collection-" + std::to_string(suffix) + ".toml");
    struct RemoveFile {
        std::filesystem::path path;
        ~RemoveFile() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{path};

    auto parse = [&](const std::string& body) {
        std::ofstream file(path);
        if (!file) throw std::runtime_error("cannot create temporary TOML config");
        file << "[runtime]\nmode='paper'\n"
                "[wallet]\ncredentials_file=''\n"
                "[data_collection]\n" << body;
        file.close();
        return Config::load(path.string());
    };

    if (parse("").data_collection.book_snapshot_levels != 20)
        throw std::runtime_error("book snapshot depth default changed");
    for (const int levels : std::array<int, 4>{0, 1, 5, 20}) {
        const auto cfg = parse("book_snapshot_levels=" + std::to_string(levels) + "\n");
        if (cfg.data_collection.book_snapshot_levels != levels)
            throw std::runtime_error("book snapshot depth failed to load");
    }
    for (const int levels : std::array<int, 2>{-1, 21}) {
        bool rejected = false;
        try {
            (void)parse("book_snapshot_levels=" + std::to_string(levels) + "\n");
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        if (!rejected) throw std::runtime_error("out-of-range book snapshot depth was accepted");
    }

    std::cout << "book snapshot depth config passed\n";
}
