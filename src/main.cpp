#include <iostream>
#include <string>

#if defined(HELLO_HAS_WX) && HELLO_HAS_WX
#include <wx/wx.h>
#endif

#include "hello_cross_platform/platform.h"

int main(int argc, char* argv[]) {
    std::string mode = "gui";
    
    // Check for --gui flag
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--gui") {
            mode = "gui";
            break;
        }
    }

    if (mode == "gui") {
#if defined(HELLO_HAS_WX) && HELLO_HAS_WX
        return wxEntry(argc, argv);
#else
        std::cout << "GUI mode is unavailable: wxWidgets was not found at configure time." << '\n';
        std::cout << "Reconfigure with wxWidgets installed to enable --gui." << '\n';
        return 1;
#endif
    }

    // CLI mode (optional fallback)
    std::cout << hello_cross_platform::build_message() << '\n';
    std::cout << "Detected platform: " << hello_cross_platform::detect_platform() << '\n';
    std::cout << "This program is designed to build on Windows and Linux." << '\n';
    std::cout << "\nTip: Run with --gui flag to launch GUI mode\n";
    return 0;
}