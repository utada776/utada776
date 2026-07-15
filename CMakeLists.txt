cmake_minimum_required(VERSION 3.16)

project(HelloCrossPlatform VERSION 1.0.0 LANGUAGES CXX)

option(BUILD_TESTING "Build unit tests" ON)

# Option: Use local offline dependencies or vcpkg
option(USE_LOCAL_DEPENDENCIES "Use VTK and DCMTK from external/ folder (offline mode)" ON)

if(USE_LOCAL_DEPENDENCIES)
    # VTK: use external mirrored vcpkg layout for offline usage
    set(VTK_DIR "${CMAKE_SOURCE_DIR}/external/vtk/install/share/vtk" CACHE PATH "VTK CMake config directory" FORCE)

    # DCMTK: use offline-compiled local installation
    set(DCMTK_DIR "${CMAKE_SOURCE_DIR}/external/dcmtk/install/cmake" CACHE PATH "DCMTK CMake config directory" FORCE)

    message(STATUS "Using LOCAL dependencies from external/ folder")
    message(STATUS "  VTK_DIR: ${VTK_DIR}")
    message(STATUS "  DCMTK_DIR: ${DCMTK_DIR}")
else()
    message(STATUS "Using vcpkg dependencies (requires CMake toolchain)")
endif()

# Find wxWidgets
set(HELLO_WX_LIBS "")
set(HELLO_GUI_AVAILABLE OFF)

# 1) Prefer package config mode (e.g. vcpkg)
find_package(wxWidgets CONFIG QUIET)
if(TARGET wx::core)
    list(APPEND HELLO_WX_LIBS wx::core wx::base)
    set(HELLO_GUI_AVAILABLE ON)
elseif(TARGET unofficial::wxwidgets::wxwidgets)
    list(APPEND HELLO_WX_LIBS unofficial::wxwidgets::wxwidgets)
    set(HELLO_GUI_AVAILABLE ON)
else()
    # 2) Fallback to classic FindwxWidgets module mode
    find_package(wxWidgets QUIET COMPONENTS core base)
    if(wxWidgets_FOUND)
        include(${wxWidgets_USE_FILE})
        list(APPEND HELLO_WX_LIBS ${wxWidgets_LIBRARIES})
        set(HELLO_GUI_AVAILABLE ON)
    endif()
endif()

if(NOT HELLO_WX_LIBS)
    message(WARNING
        "wxWidgets not found; GUI targets will be disabled.\n"
        "Only CLI build of hello_cross_platform will be generated."
    )
endif()

# Find VTK for 3D DICOM rendering (only required for GUI build)
set(HELLO_VTK_AVAILABLE OFF)
if(HELLO_GUI_AVAILABLE)
    if(NOT USE_LOCAL_DEPENDENCIES)
        # vcpkg mode: ensure VTK_DIR is set by toolchain
        if(NOT VTK_DIR)
            message(WARNING "VTK_DIR not set. When using vcpkg, pass -DCMAKE_TOOLCHAIN_FILE")
        endif()
    endif()

    # Create stub IMPORTED targets to satisfy VTK's dependencies only in pure
    # offline mode. When a vcpkg toolchain is active, let vcpkg create the real
    # imported targets; otherwise its wrappers can see conflicting targets.
    if(CMAKE_TOOLCHAIN_FILE MATCHES "vcpkg")
        find_package(Freetype CONFIG QUIET)
        find_package(GLEW CONFIG QUIET)
    else()
        if(NOT TARGET Freetype::Freetype)
            add_library(Freetype::Freetype INTERFACE IMPORTED)
        endif()
        if(NOT TARGET GLEW::GLEW)
            add_library(GLEW::GLEW INTERFACE IMPORTED)
        endif()
    endif()

    find_package(VTK CONFIG QUIET COMPONENTS
        CommonCore
        CommonDataModel
        IOImage
        InteractionStyle
        RenderingAnnotation
        RenderingCore
        RenderingImage
        RenderingOpenGL2
        RenderingVolume
    )

    if(NOT VTK_FOUND)
        if(USE_LOCAL_DEPENDENCIES)
            message(FATAL_ERROR
                "VTK not found in local external/ folder.\n"
                "  Please run: cd external && .\\BUILD_ALL_OFFLINE.ps1\n"
                "  VTK_DIR: ${VTK_DIR}\n"
                "  Expected: ${VTK_DIR}/VTKConfig.cmake"
            )
        else()
            set(HELLO_VTK_TRIPLET_HINT "x64-windows")
            message(FATAL_ERROR
                "VTK not found via vcpkg.\n"
                "  Windows (vcpkg): vcpkg install vtk:${HELLO_VTK_TRIPLET_HINT}\n"
                "  Ubuntu: sudo apt-get install libvtk9-dev\n"
                "  Details: ${VTK_NOT_FOUND_MESSAGE}\n"
                "  Then reconfigure CMake."
            )
        endif()
    endif()

    set(HELLO_VTK_AVAILABLE ON)
    message(STATUS "VTK found: ${VTK_VERSION}")
endif()

# Optional RTK backend for cone-beam FDK reconstruction.
# RTK is normally used from its build tree because RTK itself is an ITK external
# module. Point RTK_DIR at external/rtk/build or pass -DRTK_DIR=... manually.
set(HELLO_RTK_AVAILABLE OFF)
set(HELLO_RTK_LIBS "")
set(HELLO_RTK_INCLUDE_DIRS "")
if(NOT RTK_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/external/rtk/build/RTKConfig.cmake")
    set(RTK_DIR "${CMAKE_SOURCE_DIR}/external/rtk/build" CACHE PATH "RTK build-tree CMake config directory" FORCE)
endif()
find_package(RTK CONFIG QUIET)
if(RTK_FOUND)
    set(HELLO_RTK_AVAILABLE ON)
    if(ITK_USE_FILE)
        include(${ITK_USE_FILE})
    endif()
    if(RTK_LIBRARIES)
        list(APPEND HELLO_RTK_LIBS ${RTK_LIBRARIES})
    elseif(TARGET RTK)
        list(APPEND HELLO_RTK_LIBS RTK)
    endif()
    if(ITK_LIBRARIES)
        list(APPEND HELLO_RTK_LIBS ${ITK_LIBRARIES})
    endif()
    message(STATUS "RTK found: ${RTK_VERSION_MAJOR}.${RTK_VERSION_MINOR}.${RTK_VERSION_PATCH}")
else()
    set(_HELLO_RTK_BUILD_DIR "${RTK_DIR}")
    if(NOT _HELLO_RTK_BUILD_DIR)
        set(_HELLO_RTK_BUILD_DIR "${CMAKE_SOURCE_DIR}/external/rtk/build")
    endif()
    if(EXISTS "${_HELLO_RTK_BUILD_DIR}/lib/Release/itkRTK-5.3.lib")
        if(NOT ITK_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/external/itk/build/ITKConfig.cmake")
            set(ITK_DIR "${CMAKE_SOURCE_DIR}/external/itk/build" CACHE PATH "ITK build-tree CMake config directory" FORCE)
        endif()
        find_package(ITK CONFIG QUIET)
        find_library(HELLO_RTK_CORE_LIB NAMES itkRTK-5.3 itkRTK PATHS "${_HELLO_RTK_BUILD_DIR}/lib/Release" NO_DEFAULT_PATH)
        find_library(HELLO_RTK_LPSOLVE_LIB NAMES itklpsolve55-5.3 itklpsolve55 PATHS "${_HELLO_RTK_BUILD_DIR}/lib/Release" NO_DEFAULT_PATH)
        if(ITK_FOUND AND HELLO_RTK_CORE_LIB AND HELLO_RTK_LPSOLVE_LIB)
            set(HELLO_RTK_AVAILABLE ON)
            if(ITK_USE_FILE)
                include(${ITK_USE_FILE})
            endif()
            list(APPEND HELLO_RTK_LIBS ${HELLO_RTK_CORE_LIB} ${HELLO_RTK_LPSOLVE_LIB} ${ITK_LIBRARIES})
            list(APPEND HELLO_RTK_INCLUDE_DIRS
                "${CMAKE_SOURCE_DIR}/external/rtk/src/include"
                "${_HELLO_RTK_BUILD_DIR}/include"
                "${CMAKE_SOURCE_DIR}/external/rtk/src/utilities/lp_solve"
                "${CMAKE_SOURCE_DIR}/external/rtk/src/utilities/lp_solve/shared"
                "${CMAKE_SOURCE_DIR}/external/rtk/src/utilities/lp_solve/bfp"
                "${CMAKE_SOURCE_DIR}/external/rtk/src/utilities/lp_solve/bfp/bfp_LUSOL"
                "${CMAKE_SOURCE_DIR}/external/rtk/src/utilities/lp_solve/bfp/bfp_LUSOL/LUSOL"
                "${CMAKE_SOURCE_DIR}/external/rtk/src/utilities/lp_solve/colamd"
            )
            message(STATUS "RTK build tree found: ${_HELLO_RTK_BUILD_DIR}")
        endif()
    endif()
    if(NOT HELLO_RTK_AVAILABLE)
        message(WARNING
            "RTK not found; RTK FDK UI will build, but reconstruction will report that the backend is unavailable.\n"
            "  Expected build-tree config: external/rtk/build/RTKConfig.cmake\n"
            "  Or configure with: -DRTK_DIR=<path-to-rtk-build>"
        )
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable parallel PDB writes for MSVC (allows multiple CL.EXE to share PDB file)
if(MSVC)
    add_compile_options(/FS)
endif()

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

if(HELLO_GUI_AVAILABLE)
    add_executable(hello_cross_platform
        src/main.cpp
        src/gui_app.cpp
        src/dicom/dicom_viewer.cpp
        src/dicom/dicom_exporter.cpp
        src/dicom/raw_loader.cpp
        src/dicom/scan_loader.cpp
        src/volume/raw_volume_loader_dialog.cpp
        src/volume/scan_volume_loader_dialog.cpp
        src/pps/pps.cpp
        src/bean/bean.cpp
        src/registration/compatibility.cpp
        src/registration/registration.cpp
        src/registration/registration_main.cpp
        src/registration/simplex.cpp
        src/registration/transform.cpp
        src/registration/updatable.cpp
        "src/FDK recon/fdk_recon.cpp"
        "src/FDK recon/fdk_main.cpp"
        "src/FDK recon/fdk_dicom_exporter.cpp"
    )

    target_link_libraries(hello_cross_platform
        PRIVATE
            hello_core
            ${HELLO_WX_LIBS}
            ${VTK_LIBRARIES}
            ${HELLO_RTK_LIBS}
    )
    target_compile_definitions(hello_cross_platform PRIVATE HELLO_HAS_WX=1)
    target_compile_definitions(hello_cross_platform PRIVATE HELLO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    if(HELLO_RTK_AVAILABLE)
        target_compile_definitions(hello_cross_platform PRIVATE FDK_HAS_RTK=1)
    endif()
else()
    add_executable(hello_cross_platform
        src/main.cpp
    )

    target_link_libraries(hello_cross_platform
        PRIVATE
            hello_core
    )
    target_compile_definitions(hello_cross_platform PRIVATE HELLO_HAS_WX=0)
endif()

# Optional DCMTK for DICOM export
find_package(DCMTK CONFIG QUIET)
if(DCMTK_FOUND)
    message(STATUS "DCMTK found: ${DCMTK_VERSION}")
    target_compile_definitions(hello_cross_platform PRIVATE FDK_HAS_DCMTK=1)
    target_link_libraries(hello_cross_platform PRIVATE DCMTK::dcmdata DCMTK::dcmimgle DCMTK::ofstd)
    target_include_directories(hello_cross_platform PRIVATE ${DCMTK_INCLUDE_DIRS})
else()
    if(USE_LOCAL_DEPENDENCIES)
        message(WARNING 
            "DCMTK not found in local external/ folder.\n"
            "  Optional: run 'cd external && .\\BUILD_ALL_OFFLINE.ps1' to enable DICOM export\n"
            "  DCMTK_DIR: ${DCMTK_DIR}"
        )
    else()
        message(WARNING 
            "DCMTK not found in vcpkg.\n"
            "  Optional: vcpkg install dcmtk:x64-windows\n"
            "  Compilation will proceed without DICOM export support."
        )
    endif()
endif()

if(HELLO_GUI_AVAILABLE AND COMMAND vtk_module_autoinit)
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
target_include_directories(hello_cross_platform
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${HELLO_RTK_INCLUDE_DIRS}
)

if(WIN32 AND TARGET hello_cross_platform)
    if(EXISTS "${PROJECT_SOURCE_DIR}/external/dcmtk/install/bin")
        add_custom_command(TARGET hello_cross_platform POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_SOURCE_DIR}/external/dcmtk/install/bin"
                "$<TARGET_FILE_DIR:hello_cross_platform>"
            VERBATIM)
    endif()

    if(EXISTS "${PROJECT_SOURCE_DIR}/external/vtk/install/bin")
        add_custom_command(TARGET hello_cross_platform POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_SOURCE_DIR}/external/vtk/install/bin"
                "$<TARGET_FILE_DIR:hello_cross_platform>"
            VERBATIM)
    endif()

    if(EXISTS "${PROJECT_SOURCE_DIR}/assets")
        add_custom_command(TARGET hello_cross_platform POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${PROJECT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:hello_cross_platform>/assets"
            VERBATIM)
    endif()
endif()

if(MSVC)
    foreach(_hello_target hello_core hello_cross_platform)
        if(TARGET ${_hello_target})
            target_compile_options(${_hello_target} PRIVATE /utf-8 /wd4819 /wd4996 /W4 /permissive-)
            target_compile_definitions(${_hello_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
    endforeach()
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
    gtest_discover_tests(hello_core_test DISCOVERY_TIMEOUT 60)

    if(HELLO_GUI_AVAILABLE AND HELLO_VTK_AVAILABLE)
        add_executable(dicom_viewer_test
            tests/dicom_viewer_test.cpp
            src/dicom/dicom_viewer.cpp
            src/dicom/scan_loader.cpp
        )

        target_include_directories(dicom_viewer_test
            PRIVATE
                ${PROJECT_SOURCE_DIR}/src
        )

        target_link_libraries(dicom_viewer_test
            PRIVATE
                ${VTK_LIBRARIES}
                GTest::gtest_main
        )

        target_compile_features(dicom_viewer_test PRIVATE cxx_std_17)
        gtest_discover_tests(dicom_viewer_test DISCOVERY_TIMEOUT 60)
    endif()

    if(HELLO_GUI_AVAILABLE)
        add_executable(gui_app_test
            tests/gui_app_test.cpp
            src/gui_app.cpp
            src/bean/bean.cpp
            src/pps/pps.cpp
            src/dicom/dicom_viewer.cpp
            src/dicom/dicom_exporter.cpp
            src/dicom/raw_loader.cpp
            src/dicom/scan_loader.cpp
            src/volume/raw_volume_loader_dialog.cpp
            src/volume/scan_volume_loader_dialog.cpp
            "src/FDK recon/fdk_main.cpp"
            "src/FDK recon/fdk_recon.cpp"
            "src/FDK recon/fdk_dicom_exporter.cpp"
            src/registration/compatibility.cpp
            src/registration/registration.cpp
            src/registration/registration_main.cpp
            src/registration/simplex.cpp
            src/registration/transform.cpp
            src/registration/updatable.cpp
        )

        target_include_directories(gui_app_test
            PRIVATE
                ${PROJECT_SOURCE_DIR}/src
                ${PROJECT_SOURCE_DIR}/include
                ${HELLO_RTK_INCLUDE_DIRS}
        )

        target_link_libraries(gui_app_test
            PRIVATE
                hello_core
                ${HELLO_WX_LIBS}
                ${VTK_LIBRARIES}
                ${HELLO_RTK_LIBS}
                GTest::gtest_main
        )

            if(HELLO_RTK_AVAILABLE)
                target_compile_definitions(gui_app_test PRIVATE FDK_HAS_RTK=1)
            endif()

        target_compile_features(gui_app_test PRIVATE cxx_std_17)
        if(MSVC)
            target_compile_options(gui_app_test PRIVATE /utf-8 /wd4819 /wd4996 /permissive-)
            target_compile_definitions(gui_app_test PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        gtest_discover_tests(gui_app_test DISCOVERY_TIMEOUT 60)
    endif()

    if(HELLO_GUI_AVAILABLE)
        add_executable(bean_test
            tests/bean_test.cpp
            src/bean/bean.cpp
        )

        target_include_directories(bean_test
            PRIVATE
                ${PROJECT_SOURCE_DIR}/src
        )

        target_link_libraries(bean_test
            PRIVATE
                ${HELLO_WX_LIBS}
                GTest::gtest_main
        )

        target_compile_features(bean_test PRIVATE cxx_std_17)
        gtest_discover_tests(bean_test DISCOVERY_TIMEOUT 60)
    endif()
endif()

install(TARGETS hello_core hello_cross_platform
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(FILES include/hello_cross_platform/platform.h
    DESTINATION include/hello_cross_platform
)
