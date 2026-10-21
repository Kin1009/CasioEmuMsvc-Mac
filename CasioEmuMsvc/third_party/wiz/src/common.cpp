// common.cpp - file helpers shared by the toolchain.
#include "common.hpp"

#include <fstream>

namespace wiz {

std::string read_text_file(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (ok) *ok = false;
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ok) *ok = true;
    return data;
}

std::vector<uint8_t> read_binary_file(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (ok) *ok = false;
        return {};
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (ok) *ok = true;
    return std::vector<uint8_t>(data.begin(), data.end());
}

bool write_text_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(data.data(), (std::streamsize)data.size());
    return out.good();
}

bool write_binary_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    if (!data.empty()) out.write((const char*)data.data(), (std::streamsize)data.size());
    return out.good();
}

}  // namespace wiz
