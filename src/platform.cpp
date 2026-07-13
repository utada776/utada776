#include "hello_cross_platform/platform.h"

namespace hello_cross_platform {

std::string detect_platform() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__APPLE__)
    return "macOS";
#else
    return "Unknown";
#endif
}

std::string build_message() {
    return "Hello from a CMake C++ project.";
}

}  // namespace hello_cross_platform