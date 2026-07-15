// ============================================================================
// dicom_exporter.cpp
//
// DCMTK-based DICOM CT volume export.
// The real exporter is compiled only when FDK_HAS_DCMTK=1. Otherwise this file
// provides a stub with an actionable error message so the GUI can report the
// missing optional dependency instead of failing to link.
// ============================================================================

#include "dicom_exporter.h"

#ifdef FDK_HAS_DCMTK

#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcxfer.h>
#include <dcmtk/ofstd/ofstring.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

namespace dicom_export {
namespace {

// Generate a DICOM UID using DCMTK's built-in generator.
std::string GenUID() {
    char buf[128] = {};
    dcmGenerateUniqueIdentifier(buf, SITE_INSTANCE_UID_ROOT);
    return std::string(buf);
}

// Format a float with 6 decimal places for DICOM DS values.
std::string FmtDS(float v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6) << v;
    return ss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
bool ExportFdkVolume(const fdk::Volume3D&  vol,
                     const fdk::FdkParams& params,
                     const std::string&    out_dir,
                     std::string&          error)
{
    if (!vol.valid()) {
        error = "Volume is not valid.";
        return false;
    }

    // Create output directory.
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) {
        error = "Cannot create output directory: " + out_dir + " (" + ec.message() + ")";
        return false;
    }

    // UIDs shared across the series.
    const std::string study_uid  = GenUID();
    const std::string series_uid = GenUID();

    // Geometry
    const float pixel_spacing_mm = params.voxel_size_cm * 10.0f;
    const std::string spacing_str    = FmtDS(pixel_spacing_mm) + "\\" + FmtDS(pixel_spacing_mm);
    const std::string thickness_str  = FmtDS(pixel_spacing_mm);
    // Standard axial orientation: row = +X, col = +Y
    const std::string orient_str = "1\\0\\0\\0\\1\\0";

    const int cols     = vol.nx;
    const int rows     = vol.ny;
    const int n_slices = vol.nz;

    if (cols <= 0 || rows <= 0 || n_slices <= 0) {
        error = "Invalid volume dimensions for DICOM export.";
        return false;
    }
    if (cols > 65535 || rows > 65535) {
        error = "DICOM export does not support dimensions > 65535 for Rows/Columns.";
        return false;
    }

    std::vector<Uint16> px_buf(static_cast<std::size_t>(rows * cols));

    for (int iz = 0; iz < n_slices; ++iz) {
        // Convert raw volume values to HU (int16) using scale/offset from params.
        for (int iy = 0; iy < rows; ++iy) {
            for (int ix = 0; ix < cols; ++ix) {
                const float raw    = vol.at(ix, iy, iz);
                const float hu     = raw * params.scale_out + params.offset_out;
                const float clamped = std::clamp(hu, -32768.0f, 32767.0f);
                const int16_t s16 = static_cast<int16_t>(std::lround(clamped));
                // PixelRepresentation=1 means signed interpretation; store raw 16-bit pattern in OW.
                px_buf[static_cast<std::size_t>(iy * cols + ix)] =
                    static_cast<Uint16>(static_cast<uint16_t>(s16));
            }
        }

        const std::string sop_uid = GenUID();

        // Image position: centre of volume at (0,0); Z progresses by spacing.
        const std::string pos_str =
            "0\\0\\" + FmtDS(static_cast<float>(iz) * pixel_spacing_mm);

        // ---- Build DICOM dataset ----
        DcmFileFormat ff;
        DcmDataset*   ds = ff.getDataset();

        // Patient
        ds->putAndInsertString(DCM_PatientName,  "FDK Reconstruction");
        ds->putAndInsertString(DCM_PatientID,    "FDK001");
        ds->putAndInsertString(DCM_PatientBirthDate, "");
        ds->putAndInsertString(DCM_PatientSex,   "");

        // Study
        ds->putAndInsertString(DCM_StudyInstanceUID,  study_uid.c_str());
        ds->putAndInsertString(DCM_StudyDate,         "");
        ds->putAndInsertString(DCM_StudyTime,         "");
        ds->putAndInsertString(DCM_StudyDescription,  "FDK Cone-Beam CT Reconstruction");
        ds->putAndInsertString(DCM_StudyID,           "1");

        // Series
        ds->putAndInsertString(DCM_SeriesInstanceUID, series_uid.c_str());
        ds->putAndInsertString(DCM_SeriesNumber,      "1");
        ds->putAndInsertString(DCM_SeriesDescription, "FDK Volume");
        ds->putAndInsertString(DCM_Modality,          "CT");

        // SOP
        ds->putAndInsertString(DCM_SOPClassUID,    UID_CTImageStorage);
        ds->putAndInsertString(DCM_SOPInstanceUID, sop_uid.c_str());

        // Image plane
        ds->putAndInsertString(DCM_ImageOrientationPatient, orient_str.c_str());
        ds->putAndInsertString(DCM_ImagePositionPatient,    pos_str.c_str());
        ds->putAndInsertString(DCM_PixelSpacing,            spacing_str.c_str());
        ds->putAndInsertString(DCM_SliceThickness,          thickness_str.c_str());
        ds->putAndInsertSint32(DCM_InstanceNumber,
                               static_cast<Sint32>(iz + 1));

        // Image pixel
        ds->putAndInsertUint16(DCM_SamplesPerPixel,        1);
        ds->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
        ds->putAndInsertUint16(DCM_Rows,
                               static_cast<Uint16>(rows));
        ds->putAndInsertUint16(DCM_Columns,
                               static_cast<Uint16>(cols));
        ds->putAndInsertUint16(DCM_BitsAllocated,  16);
        ds->putAndInsertUint16(DCM_BitsStored,     16);
        ds->putAndInsertUint16(DCM_HighBit,        15);
        ds->putAndInsertUint16(DCM_PixelRepresentation, 1); // signed

        // Rescale: pixels already in HU
        ds->putAndInsertString(DCM_RescaleIntercept, "0");
        ds->putAndInsertString(DCM_RescaleSlope,     "1");
        ds->putAndInsertString(DCM_RescaleType,      "HU");

        // Pixel data (OW). Use Uint16 payload; signedness is defined by PixelRepresentation.
        const OFCondition pxcond = ds->putAndInsertUint16Array(
            DCM_PixelData,
            px_buf.data(),
            static_cast<unsigned long>(px_buf.size()));
        if (pxcond.bad()) {
            error = "Failed to set pixel data for slice " +
                    std::to_string(iz) + ": " + pxcond.text();
            return false;
        }

        // Write file.
        const std::string fname =
            out_dir + "/IM_" + std::to_string(iz + 1) + ".dcm";
        const OFCondition wcond =
            ff.saveFile(fname.c_str(), EXS_LittleEndianExplicit);
        if (wcond.bad()) {
            error = "DCMTK write failed for slice " + std::to_string(iz) +
                    " -> " + fname + ": " + wcond.text();
            return false;
        }
    }

    return true;
}

} // namespace dicom_export

// ============================================================================
#else // FDK_HAS_DCMTK not defined
// ============================================================================

namespace dicom_export {

bool ExportFdkVolume(const fdk::Volume3D&,
                     const fdk::FdkParams&,
                     const std::string&,
                     std::string& error)
{
    error =
        "DCMTK is not available in this build.\n"
        "To enable DICOM export (offline local), run:\n"
        "  cd external\n"
        "  .\\BUILD_ALL_OFFLINE.ps1 -SkipVTK -SkipDownload\n"
        "Then reconfigure CMake and rebuild.\n"
        "(Alternative: vcpkg install dcmtk:x64-windows)";
    return false;
}

} // namespace dicom_export

#endif // FDK_HAS_DCMTK
