cmake_minimum_required(VERSION 3.16)

project(HelloCrossPlatform VERSION 1.0.0 LANGUAGES CXX)

# Find wxWidgets
set(HELLO_WX_LIBS "")

# 1) Prefer package config mode (e.g. vcpkg)
find_package(wxWidgets CONFIG QUIET)
if(TARGET wx::core)
    list(APPEND HELLO_WX_LIBS wx::core wx::base)
elseif(TARGET unofficial::wxwidgets::wxwidgets)
    list(APPEND HELLO_WX_LIBS unofficial::wxwidgets::wxwidgets)
else()
    # 2) Fallback to classic FindwxWidgets module mode
    find_package(wxWidgets QUIET COMPONENTS core base)
    if(wxWidgets_FOUND)
        include(${wxWidgets_USE_FILE})
        list(APPEND HELLO_WX_LIBS ${wxWidgets_LIBRARIES})
    endif()
endif()

if(NOT HELLO_WX_LIBS)
    set(HELLO_VCPKG_TRIPLET_HINT "x64-windows")

    message(FATAL_ERROR
        "wxWidgets not found.\n"
        "  Windows (vcpkg): vcpkg install wxwidgets:${HELLO_VCPKG_TRIPLET_HINT}\n"
        "    then configure with -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake\n"
        "  Ubuntu: sudo apt-get install libwxgtk3.2-dev (or libwxgtk3.0-gtk3-dev)\n"
        "  You can also set wxWidgets_ROOT_DIR / wxWidgets_LIB_DIR for module mode."
    )
endif()

# Find VTK for 3D DICOM rendering
find_package(VTK CONFIG QUIET COMPONENTS
    CommonCore
    CommonDataModel
    IOImage
    InteractionStyle
    RenderingCore
    RenderingOpenGL2
    RenderingVolume
)

if(NOT VTK_FOUND)
    set(HELLO_VTK_TRIPLET_HINT "x64-windows")

    message(FATAL_ERROR
        "VTK not found.\n"
        "  Windows (vcpkg): vcpkg install vtk:${HELLO_VTK_TRIPLET_HINT}\n"
        "  Ubuntu: sudo apt-get install libvtk9-dev\n"
        "  Details: ${VTK_NOT_FOUND_MESSAGE}\n"
        "  Then reconfigure CMake."
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HELLO_BUILD_SHARED "Build hello_core as a shared library" OFF)

if(HELLO_BUILD_SHARED)
    set(hello_core_type SHARED)
else()
    set(hello_core_type STATIC)
endif()

add_library(hello_core ${hello_core_type}
    src/platform.cpp
)

target_include_directories(hello_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

add_executable(hello_cross_platform
    src/main.cpp
    src/gui_app.cpp
    src/gui_app.h
    src/dicom_viewer.cpp
    src/dicom_viewer.h
)

target_link_libraries(hello_cross_platform
    PRIVATE
        hello_core
        ${HELLO_WX_LIBS}
        ${VTK_LIBRARIES}
)

if(COMMAND vtk_module_autoinit)
    vtk_module_autoinit(
        TARGETS hello_cross_platform
        MODULES ${VTK_LIBRARIES}
    )
endif()

set_target_properties(hello_core PROPERTIES
    OUTPUT_NAME hello_core
)

set_target_properties(hello_cross_platform PROPERTIES
    OUTPUT_NAME hello_cross_platform
)

target_compile_features(hello_core PUBLIC cxx_std_17)
target_compile_features(hello_cross_platform PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(hello_core PRIVATE /W4 /permissive-)
    target_compile_options(hello_cross_platform PRIVATE /W4 /permissive-)
else()
    target_compile_options(hello_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(hello_cross_platform PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)

if(BUILD_TESTING)
    # Keep FetchContent downloads in the project so they are reused across
    # re-configures and different build directories.
    set(FETCHCONTENT_BASE_DIR "${PROJECT_SOURCE_DIR}/.deps" CACHE PATH "FetchContent base directory")
    # Prevent automatic remote update checks after first successful download.
    set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "Disable FetchContent updates" FORCE)

    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
        GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(hello_core_test
        tests/hello_core_test.cpp
    )

    target_link_libraries(hello_core_test
        PRIVATE
            hello_core
            GTest::gtest_main
    )

    target_compile_features(hello_core_test PRIVATE cxx_std_17)

    include(GoogleTest)
    gtest_discover_tests(hello_core_test)
endif()

install(TARGETS hello_core hello_cross_platform
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES include/hello_cross_platform/platform.h
    DESTINATION include/hello_cross_platform
)