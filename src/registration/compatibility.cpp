#include "registration/compatibility.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>

namespace registration {

// 该文件保持“薄工具层”定位：尽量只做跨平台与基础转换，不承载业务逻辑。

int sign(int value) {
    if (value > 0) {
        return 1;
    }
    if (value < 0) {
        return -1;
    }
    return 0;
}

int sign(std::int64_t value) {
    if (value > 0) {
        return 1;
    }
    if (value < 0) {
        return -1;
    }
    return 0;
}

int sign(double value) {
    if (value > 0.0) {
        return 1;
    }
    if (value < 0.0) {
        return -1;
    }
    return 0;
}

bool in_range(int value, int min_value, int max_value) {
    return value >= min_value && value <= max_value;
}

bool in_range(double value, double min_value, double max_value) {
    return value >= min_value && value <= max_value;
}

int ensure_range(int value, int min_value, int max_value) {
    return std::clamp(value, min_value, max_value);
}

double ensure_range(double value, double min_value, double max_value) {
    return std::clamp(value, min_value, max_value);
}

double str_to_double_def(const std::string& s, double default_value) {
    double out = default_value;
    const char* begin = s.data();
    const char* end = s.data() + s.size();
    auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc() || result.ptr != end) {
        return default_value;
    }
    return out;
}

std::string include_trailing_path_delimiter(const std::string& s) {
#ifdef _WIN32
    constexpr char kSep = '\\';
#else
    constexpr char kSep = '/';
#endif

    if (s.empty()) {
        return std::string(1, kSep);
    }
    const char last = s.back();
    if (last == '/' || last == '\\') {
        return s;
    }
    std::string out = s;
    out.push_back(kSep);
    return out;
}

std::string exclude_trailing_path_delimiter(const std::string& s) {
    if (s.empty()) {
        return s;
    }
    std::string out = s;
    while (!out.empty() && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    return out;
}

bool directory_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(std::filesystem::path(path), ec);
}

std::string get_environment_variable(const std::string& name) {
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name.c_str()) != 0 || buffer == nullptr) {
        return std::string();
    }
    std::string out(buffer);
    std::free(buffer);
    return out;
#else
    const char* v = std::getenv(name.c_str());
    return v == nullptr ? std::string() : std::string(v);
#endif
}

}  // namespace registration


