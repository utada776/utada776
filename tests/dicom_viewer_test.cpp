#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "dicom/dicom_viewer.h"

namespace {

std::filesystem::path MakeTempDir(const std::string& suffix) {
    const auto base = std::filesystem::temp_directory_path();
    const auto path = base / ("dicom_viewer_test_" + suffix);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
    return path;
}

}  // namespace

TEST(DicomViewerTest, InvalidPathFailsValidation) {
    std::string error;
    const bool ok = dicom_viewer::ValidateDicomDirectory(
        "C:/this/path/should/not/exist_123456", error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(DicomViewerTest, EmptyDirectoryFailsValidation) {
    const auto dir = MakeTempDir("empty");

    std::string error;
    const bool ok = dicom_viewer::ValidateDicomDirectory(dir.string(), error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}

TEST(DicomViewerTest, DicomdirOnlyFailsValidation) {
    const auto dir = MakeTempDir("dicomdir_only");
    const auto file = dir / "DICOMDIR";

    std::ofstream out(file.string(), std::ios::binary);
    out << "not_a_slice";
    out.close();

    std::string error;
    const bool ok = dicom_viewer::ValidateDicomDirectory(dir.string(), error);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(error.empty());
}
