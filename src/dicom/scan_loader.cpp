#include "dicom/scan_loader.h"

#include <vtkImageData.h>
#include <vtkNew.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AVS Field header parser
// ---------------------------------------------------------------------------
namespace {

struct AvsFieldHeader {
    int dim[3] = {0, 0, 0};  // dim1, dim2, dim3
    double min_ext[3] = {0.0, 0.0, 0.0};
    double max_ext[3] = {0.0, 0.0, 0.0};
    bool has_ext = false;
    std::string data_type;  // e.g. "xdr_short"
    std::streampos data_offset = 0;
};

// Trim leading/trailing whitespace from a string.
static std::string Trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Parse the ASCII header of an AVS Field file.
// The header ends at the first pair of form-feed characters (0x0C 0x0C).
// Returns false and sets error on failure.
static bool ParseAvsHeader(
    std::ifstream& f,
    AvsFieldHeader& hdr,
    std::string& err) {

    // Read header bytes until we find 0x0C 0x0C
    std::string header_text;
    header_text.reserve(1024);
    int prev_ch = -1;
    while (true) {
        const int ch = f.get();
        if (ch == std::ifstream::traits_type::eof()) {
            err = "Unexpected end of file while reading SCAN header";
            return false;
        }
        if (ch == 0x0C && prev_ch == 0x0C) {
            // Two consecutive form-feeds = end of header
            // Remove trailing 0x0C already added to header_text
            if (!header_text.empty()) {
                header_text.pop_back();
            }
            break;
        }
        header_text.push_back(static_cast<char>(ch));
        prev_ch = ch;
    }

    // Record the data start offset
    hdr.data_offset = f.tellg();

    // Parse key=value lines
    std::istringstream ss(header_text);
    std::string line;
    while (std::getline(ss, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, eq));
        const std::string val = Trim(line.substr(eq + 1));

        if (key == "dim1") {
            hdr.dim[0] = std::stoi(val);
        } else if (key == "dim2") {
            hdr.dim[1] = std::stoi(val);
        } else if (key == "dim3") {
            hdr.dim[2] = std::stoi(val);
        } else if (key == "data") {
            hdr.data_type = val;
        } else if (key == "min_ext") {
            std::istringstream vs(val);
            vs >> hdr.min_ext[0] >> hdr.min_ext[1] >> hdr.min_ext[2];
            hdr.has_ext = true;
        } else if (key == "max_ext") {
            std::istringstream vs(val);
            vs >> hdr.max_ext[0] >> hdr.max_ext[1] >> hdr.max_ext[2];
        }
    }

    if (hdr.dim[0] <= 0 || hdr.dim[1] <= 0 || hdr.dim[2] <= 0) {
        err = "SCAN header: invalid or missing dimensions (dim1/dim2/dim3)";
        return false;
    }
    if (hdr.data_type.empty()) {
        err = "SCAN header: missing 'data' field";
        return false;
    }

    return true;
}

// Swap bytes of a 16-bit value (little-endian ↔ big-endian)
static inline uint16_t ByteSwap16(uint16_t v) {
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

// Detect host endianness at runtime
static bool IsLittleEndian() {
    const uint16_t test = 0x0001u;
    uint8_t byte;
    std::memcpy(&byte, &test, 1);
    return byte == 0x01u;
}

}  // namespace

namespace scan_volume {

vtkSmartPointer<vtkImageData> LoadScanVolume(
    const ScanVolumeMetadata& metadata,
    std::string& out_error) {

    namespace fs = std::filesystem;

    if (metadata.file_path.empty()) {
        out_error = "No SCAN file path provided";
        return nullptr;
    }

    std::error_code ec;
    if (!fs::exists(metadata.file_path, ec)) {
        out_error = "File not found: " + metadata.file_path;
        return nullptr;
    }

    std::ifstream file(metadata.file_path, std::ios::binary);
    if (!file.is_open()) {
        out_error = "Failed to open file: " + metadata.file_path;
        return nullptr;
    }

    // Parse AVS Field ASCII header
    AvsFieldHeader hdr;
    if (!ParseAvsHeader(file, hdr, out_error)) {
        return nullptr;
    }

    // Only xdr_short supported (big-endian int16)
    if (hdr.data_type != "xdr_short") {
        out_error = "Unsupported SCAN data type: '" + hdr.data_type +
                    "' (only xdr_short is supported)";
        return nullptr;
    }

    const size_t nx = static_cast<size_t>(hdr.dim[0]);
    const size_t ny = static_cast<size_t>(hdr.dim[1]);
    const size_t nz = static_cast<size_t>(hdr.dim[2]);
    const size_t num_voxels = nx * ny * nz;

    // Read big-endian int16 data
    std::vector<uint16_t> raw(num_voxels);
    file.read(reinterpret_cast<char*>(raw.data()),
              static_cast<std::streamsize>(num_voxels * sizeof(uint16_t)));

    if (!file || static_cast<size_t>(file.gcount()) != num_voxels * sizeof(uint16_t)) {
        out_error = "Failed to read complete voxel data from SCAN file";
        return nullptr;
    }
    file.close();

    // Byte-swap if host is little-endian (XDR is big-endian)
    if (IsLittleEndian()) {
        for (auto& v : raw) {
            v = ByteSwap16(v);
        }
    }

    // Compute voxel spacing from min_ext / max_ext (units match the file, typically mm)
    double spacing[3] = {1.0, 1.0, 1.0};
    if (hdr.has_ext) {
        for (int i = 0; i < 3; ++i) {
            if (hdr.dim[i] > 1) {
                spacing[i] = (hdr.max_ext[i] - hdr.min_ext[i]) /
                             static_cast<double>(hdr.dim[i] - 1);
                if (spacing[i] <= 0.0) {
                    spacing[i] = 1.0;
                }
            }
        }
    }

    // Build vtkImageData with float scalars for display
    // AVS field: dim1 = fastest-varying (column / X), dim2 = Y, dim3 = slowest (slice / Z)
    vtkNew<vtkImageData> image;
    image->SetDimensions(
        static_cast<int>(nx),
        static_cast<int>(ny),
        static_cast<int>(nz));
    image->SetSpacing(spacing[0], spacing[1], spacing[2]);
    image->AllocateScalars(VTK_FLOAT, 1);

    auto* dst = static_cast<float*>(image->GetScalarPointer());
    for (size_t i = 0; i < num_voxels; ++i) {
        dst[i] = static_cast<float>(static_cast<int16_t>(raw[i]));
    }

    return image;
}

}  // namespace scan_volume
