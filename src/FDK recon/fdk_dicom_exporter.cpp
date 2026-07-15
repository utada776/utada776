#include "FDK recon/fdk_dicom_exporter.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#ifdef FDK_HAS_DCMTK
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <dcmtk/dcmdata/dcxfer.h>
#endif

namespace fdk_dicom_exporter {

#ifdef FDK_HAS_DCMTK
namespace {
std::string Uid() { char uid[128] = {}; dcmGenerateUniqueIdentifier(uid, SITE_INSTANCE_UID_ROOT); return uid; }
std::string Ds(float v) { char buf[64]; std::snprintf(buf, sizeof(buf), "%.6f", v); return buf; }
}
#endif

bool ExportVolume(const fdk::Volume3D& volume, const fdk::FdkParams& params, const std::string& out_dir, std::string& error) {
    if (!volume.valid()) { error = "No valid reconstructed volume."; return false; }
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    if (ec) { error = ec.message(); return false; }
#ifndef FDK_HAS_DCMTK
    const std::filesystem::path raw_path = std::filesystem::path(out_dir) / "rtk_reconstruction_float32.raw";
    std::ofstream out(raw_path, std::ios::binary);
    if (!out) { error = "DCMTK is unavailable and RAW fallback could not be opened."; return false; }
    out.write(reinterpret_cast<const char*>(volume.data.data()), static_cast<std::streamsize>(volume.data.size() * sizeof(float)));
    error = "DCMTK is unavailable; wrote RAW fallback instead: " + raw_path.string();
    return true;
#else
    const std::string study = Uid();
    const std::string series = Uid();
    const float spacing_mm = params.voxel_size_cm * 10.0f;
    std::vector<Uint16> pixels(static_cast<std::size_t>(volume.nx) * volume.ny);
    for (int z = 0; z < volume.nz; ++z) {
        for (int y = 0; y < volume.ny; ++y) for (int x = 0; x < volume.nx; ++x) {
            const float hu = volume.at(x, y, z) * params.scale_out + params.offset_out;
            const auto s16 = static_cast<int16_t>(std::lround(std::clamp(hu, -32768.0f, 32767.0f)));
            pixels[static_cast<std::size_t>(y) * volume.nx + x] = static_cast<Uint16>(static_cast<uint16_t>(s16));
        }
        DcmFileFormat file;
        DcmDataset* ds = file.getDataset();
        const std::string sop = Uid();
        ds->putAndInsertString(DCM_PatientName, "RTK Reconstruction");
        ds->putAndInsertString(DCM_PatientID, "RTK001");
        ds->putAndInsertString(DCM_Modality, "CT");
        ds->putAndInsertString(DCM_StudyInstanceUID, study.c_str());
        ds->putAndInsertString(DCM_SeriesInstanceUID, series.c_str());
        ds->putAndInsertString(DCM_SOPClassUID, UID_CTImageStorage);
        ds->putAndInsertString(DCM_SOPInstanceUID, sop.c_str());
        ds->putAndInsertUint16(DCM_SamplesPerPixel, 1);
        ds->putAndInsertString(DCM_PhotometricInterpretation, "MONOCHROME2");
        ds->putAndInsertUint16(DCM_Rows, static_cast<Uint16>(volume.ny));
        ds->putAndInsertUint16(DCM_Columns, static_cast<Uint16>(volume.nx));
        ds->putAndInsertUint16(DCM_BitsAllocated, 16);
        ds->putAndInsertUint16(DCM_BitsStored, 16);
        ds->putAndInsertUint16(DCM_HighBit, 15);
        ds->putAndInsertUint16(DCM_PixelRepresentation, 1);
        const std::string spacing = Ds(spacing_mm) + "\\" + Ds(spacing_mm);
        ds->putAndInsertString(DCM_PixelSpacing, spacing.c_str());
        ds->putAndInsertString(DCM_SliceThickness, Ds(spacing_mm).c_str());
        ds->putAndInsertSint32(DCM_InstanceNumber, z + 1);
        ds->putAndInsertUint16Array(DCM_PixelData, pixels.data(), static_cast<unsigned long>(pixels.size()));
        const auto path = (std::filesystem::path(out_dir) / ("IM_" + std::to_string(z + 1) + ".dcm")).string();
        const auto cond = file.saveFile(path.c_str(), EXS_LittleEndianExplicit);
        if (cond.bad()) { error = cond.text(); return false; }
    }
    return true;
#endif
}

} // namespace fdk_dicom_exporter
