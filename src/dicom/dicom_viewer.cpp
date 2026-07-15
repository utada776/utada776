#include "dicom_viewer.h"

// ── VTK factory registration ────────────────────────────────────────────────
// Force registration explicitly (cmake-generated autoinit may fail on paths
// containing spaces under MSVC).
#include <vtkAutoInit.h>
VTK_MODULE_INIT(vtkRenderingOpenGL2)
VTK_MODULE_INIT(vtkInteractionStyle)
// ────────────────────────────────────────────────────────────────────────────

#include <vtkCamera.h>
#include <vtkDICOMImageReader.h>
#include <vtkCornerAnnotation.h>
#include <vtkImageData.h>
#include <vtkImageMapper.h>
#include <vtkSmartPointer.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>
#include <vtkPNGReader.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>
#include <vtkTextActor.h>
#include <vtkTextProperty.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>

#ifdef FDK_HAS_DCMTK
#include <dcmtk/dcmdata/dcdeftag.h>
#include <dcmtk/dcmdata/dcdatset.h>
#include <dcmtk/dcmdata/dcfilefo.h>
#include <dcmtk/ofstd/ofstring.h>
#endif

namespace dicom_viewer {

namespace {

std::filesystem::path OrientationAssetPath(char plane_code) {
    const char* file_name = "axial.png";
    if (plane_code == 'S') {
        file_name = "sagittal.png";
    } else if (plane_code == 'C') {
        file_name = "coronal.png";
    }

    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::current_path() / "assets" / "orientation" / file_name);
#ifdef HELLO_SOURCE_DIR
    candidates.push_back(std::filesystem::path(HELLO_SOURCE_DIR) / "assets" / "orientation" / file_name);
#endif
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

void AddPatientPlaneGlyph(vtkRenderer* renderer, char plane_code) {
    if (renderer == nullptr) {
        return;
    }

    const std::filesystem::path asset_path = OrientationAssetPath(plane_code);
    if (asset_path.empty()) {
        return;
    }

    vtkNew<vtkPNGReader> reader;
    reader->SetFileName(asset_path.string().c_str());
    reader->Update();
    vtkNew<vtkImageData> icon_image;
    icon_image->DeepCopy(reader->GetOutput());

    vtkNew<vtkImageMapper> mapper;
    mapper->SetInputData(icon_image);
    mapper->SetColorWindow(255.0);
    mapper->SetColorLevel(127.5);

    vtkNew<vtkActor2D> actor;
    actor->SetMapper(mapper);
    actor->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    actor->SetPosition(0.81, 0.035);
    renderer->AddActor2D(actor);
}

#ifdef FDK_HAS_DCMTK
struct DcmtkSliceInfo {
    std::filesystem::path path;
    double z = std::numeric_limits<double>::quiet_NaN();
    int instance_number = 0;
    std::vector<int16_t> pixels;
};

double ParseDicomComponent(const OFString& value, std::size_t component_index, double fallback) {
    std::stringstream ss(value.c_str());
    std::string item;
    for (std::size_t index = 0; std::getline(ss, item, '\\'); ++index) {
        if (index == component_index) {
            try {
                return std::stod(item);
            } catch (...) {
                return fallback;
            }
        }
    }
    return fallback;
}

vtkSmartPointer<vtkImageData> LoadDicomVolumeWithDcmtk(const std::string& directory, std::string& error_message) {
    namespace fs = std::filesystem;

    std::vector<DcmtkSliceInfo> slices;
    int rows = -1;
    int cols = -1;
    double spacing_x = 1.0;
    double spacing_y = 1.0;
    double spacing_z = 1.0;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }

        const auto ext = entry.path().extension().string();
        const auto stem = entry.path().stem().string();
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
        std::string lower_stem = stem;
        std::transform(lower_stem.begin(), lower_stem.end(), lower_stem.begin(), ::tolower);
        if (!(lower_ext == ".dcm" || lower_ext == ".dicom" ||
              (lower_ext.empty() && lower_stem != "dicomdir" && lower_stem != "dicom"))) {
            continue;
        }

        DcmFileFormat file_format;
        if (file_format.loadFile(entry.path().string().c_str()).bad()) {
            continue;
        }

        DcmDataset* ds = file_format.getDataset();
        Uint16 file_rows = 0;
        Uint16 file_cols = 0;
        if (ds->findAndGetUint16(DCM_Rows, file_rows).bad() || ds->findAndGetUint16(DCM_Columns, file_cols).bad()) {
            continue;
        }

        const Uint16* pixel_words = nullptr;
        unsigned long pixel_count = 0;
        if (ds->findAndGetUint16Array(DCM_PixelData, pixel_words, &pixel_count).bad() || pixel_words == nullptr) {
            continue;
        }

        const std::size_t expected_pixels = static_cast<std::size_t>(file_rows) * static_cast<std::size_t>(file_cols);
        if (pixel_count < expected_pixels) {
            continue;
        }

        if (rows < 0) {
            rows = static_cast<int>(file_rows);
            cols = static_cast<int>(file_cols);

            OFString pixel_spacing_str;
            if (ds->findAndGetOFStringArray(DCM_PixelSpacing, pixel_spacing_str).good()) {
                spacing_y = ParseDicomComponent(pixel_spacing_str, 0, spacing_y);
                spacing_x = ParseDicomComponent(pixel_spacing_str, 1, spacing_x);
            }
            OFString slice_thickness_str;
            if (ds->findAndGetOFStringArray(DCM_SliceThickness, slice_thickness_str).good()) {
                spacing_z = ParseDicomComponent(slice_thickness_str, 0, spacing_z);
            }
        } else if (rows != static_cast<int>(file_rows) || cols != static_cast<int>(file_cols)) {
            error_message = "DICOM slices in the folder do not share the same Rows/Columns.";
            return nullptr;
        }

        Uint16 pixel_representation = 0;
        ds->findAndGetUint16(DCM_PixelRepresentation, pixel_representation);

        DcmtkSliceInfo slice;
        slice.path = entry.path();
        slice.pixels.resize(expected_pixels);
        for (std::size_t i = 0; i < expected_pixels; ++i) {
            if (pixel_representation == 1) {
                slice.pixels[i] = static_cast<int16_t>(static_cast<uint16_t>(pixel_words[i]));
            } else {
                slice.pixels[i] = static_cast<int16_t>(std::min<Uint16>(pixel_words[i], 32767));
            }
        }

        Sint32 instance_number = 0;
        if (ds->findAndGetSint32(DCM_InstanceNumber, instance_number).good()) {
            slice.instance_number = static_cast<int>(instance_number);
        }
        OFString image_position_str;
        if (ds->findAndGetOFStringArray(DCM_ImagePositionPatient, image_position_str).good()) {
            slice.z = ParseDicomComponent(image_position_str, 2, slice.z);
        }

        slices.push_back(std::move(slice));
    }

    if (slices.empty()) {
        error_message = "No readable DICOM slices were found in the selected folder.";
        return nullptr;
    }

    std::sort(slices.begin(), slices.end(), [](const DcmtkSliceInfo& a, const DcmtkSliceInfo& b) {
        const bool a_has_z = std::isfinite(a.z);
        const bool b_has_z = std::isfinite(b.z);
        if (a_has_z != b_has_z) {
            return a_has_z > b_has_z;
        }
        if (a_has_z && b_has_z && a.z != b.z) {
            return a.z < b.z;
        }
        if (a.instance_number != b.instance_number) {
            return a.instance_number < b.instance_number;
        }
        return a.path.filename().string() < b.path.filename().string();
    });

    if (slices.size() >= 2 && std::isfinite(slices[0].z) && std::isfinite(slices[1].z)) {
        const double dz = std::abs(slices[1].z - slices[0].z);
        if (dz > 0.0) {
            spacing_z = dz;
        }
    }

    vtkSmartPointer<vtkImageData> image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(cols, rows, static_cast<int>(slices.size()));
    image->SetSpacing(spacing_x, spacing_y, spacing_z);
    image->AllocateScalars(VTK_SHORT, 1);

    auto* dst = static_cast<int16_t*>(image->GetScalarPointer());
    const std::size_t plane_size = static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
    for (std::size_t iz = 0; iz < slices.size(); ++iz) {
        std::memcpy(dst + iz * plane_size, slices[iz].pixels.data(), plane_size * sizeof(int16_t));
    }

    return image;
}
#endif

void ConfigureSliceCamera(vtkRenderer* renderer, vtkImageSlice* slice_actor, int orientation_axis) {
    if (renderer == nullptr || slice_actor == nullptr) {
        return;
    }

    double bounds[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    slice_actor->GetBounds(bounds);

    if (bounds[0] >= bounds[1] || bounds[2] >= bounds[3] || bounds[4] >= bounds[5]) {
        return;
    }

    const double center[3] = {
        0.5 * (bounds[0] + bounds[1]),
        0.5 * (bounds[2] + bounds[3]),
        0.5 * (bounds[4] + bounds[5])
    };

    const double size_x = std::max(1.0, bounds[1] - bounds[0]);
    const double size_y = std::max(1.0, bounds[3] - bounds[2]);
    const double size_z = std::max(1.0, bounds[5] - bounds[4]);
    const double plane_extent = std::max(size_x, std::max(size_y, size_z));
    const double distance = std::max(500.0, plane_extent * 2.5);

    renderer->ResetCamera(bounds);

    vtkCamera* camera = renderer->GetActiveCamera();
    if (camera == nullptr) {
        return;
    }

    camera->ParallelProjectionOn();
    camera->SetFocalPoint(center);

    if (orientation_axis == 0) {
        camera->SetPosition(center[0] + distance, center[1], center[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
        camera->SetParallelScale(0.5 * std::max(size_y, size_z));
    } else if (orientation_axis == 1) {
        camera->SetPosition(center[0], center[1] - distance, center[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
        camera->SetParallelScale(0.5 * std::max(size_x, size_z));
    } else {
        camera->SetPosition(center[0], center[1], center[2] + distance);
        camera->SetViewUp(0.0, -1.0, 0.0);
        camera->SetParallelScale(0.5 * std::max(size_x, size_y));
    }

    camera->OrthogonalizeViewUp();
    camera->SetClippingRange(0.1, distance * 4.0);
    renderer->ResetCameraClippingRange(bounds);
}

void ConfigureRawSliceCamera(vtkRenderer* renderer, vtkImageSlice* slice_actor, char plane_code) {
    if (renderer == nullptr || slice_actor == nullptr) {
        return;
    }

    double bounds[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    slice_actor->GetBounds(bounds);
    if (bounds[0] >= bounds[1] || bounds[2] >= bounds[3] || bounds[4] >= bounds[5]) {
        return;
    }

    const double center[3] = {
        0.5 * (bounds[0] + bounds[1]),
        0.5 * (bounds[2] + bounds[3]),
        0.5 * (bounds[4] + bounds[5])
    };

    const double size_x = std::max(1.0, bounds[1] - bounds[0]);
    const double size_y = std::max(1.0, bounds[3] - bounds[2]);
    const double size_z = std::max(1.0, bounds[5] - bounds[4]);
    const double plane_extent = std::max(size_x, std::max(size_y, size_z));
    const double distance = std::max(500.0, plane_extent * 2.5);

    renderer->ResetCamera(bounds);
    vtkCamera* camera = renderer->GetActiveCamera();
    if (camera == nullptr) {
        return;
    }

    camera->ParallelProjectionOn();
    camera->SetFocalPoint(center);

    // Raw volumes from this reconstruction workflow follow a patient-axis
    // convention where:
    //   X -> Left/Right, Y -> Superior/Inferior, Z -> Anterior/Posterior.
    // Therefore coronal view should slice normal to Z, and axial to Y.
    if (plane_code == 'S') {
        // Sagittal (normal X), with view-up set to +Y.
        // Compared to the previous +Z setup, this is a 90 deg CCW correction.
        camera->SetPosition(center[0] + distance, center[1], center[2]);
        camera->SetViewUp(0.0, 1.0, 0.0);
        camera->SetParallelScale(0.5 * std::max(size_y, size_z));
    } else if (plane_code == 'C') {
        // Coronal (normal Z), view-up +Y to keep superior at top.
        // This aligns coronal content with the former axial-content orientation.
        camera->SetPosition(center[0], center[1], center[2] + distance);
        camera->SetViewUp(0.0, 1.0, 0.0);
        camera->SetParallelScale(0.5 * std::max(size_x, size_y));
    } else {
        // Axial (normal Y), using the classic coronal camera convention.
        camera->SetPosition(center[0], center[1] - distance, center[2]);
        camera->SetViewUp(0.0, 0.0, 1.0);
        camera->SetParallelScale(0.5 * std::max(size_x, size_z));
    }

    camera->OrthogonalizeViewUp();
    camera->SetClippingRange(0.1, distance * 4.0);
    renderer->ResetCameraClippingRange(bounds);
}

void ConfigureScanSliceCamera(vtkRenderer* renderer, vtkImageSlice* slice_actor, char plane_code) {
    if (renderer == nullptr || slice_actor == nullptr) {
        return;
    }

    double bounds[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    slice_actor->GetBounds(bounds);
    if (bounds[0] >= bounds[1] || bounds[2] >= bounds[3] || bounds[4] >= bounds[5]) {
        return;
    }

    const double center[3] = {
        0.5 * (bounds[0] + bounds[1]),
        0.5 * (bounds[2] + bounds[3]),
        0.5 * (bounds[4] + bounds[5])
    };

    const double size_x = std::max(1.0, bounds[1] - bounds[0]);
    const double size_y = std::max(1.0, bounds[3] - bounds[2]);
    const double size_z = std::max(1.0, bounds[5] - bounds[4]);
    const double plane_extent = std::max(size_x, std::max(size_y, size_z));
    const double distance = std::max(500.0, plane_extent * 2.5);

    renderer->ResetCamera(bounds);
    vtkCamera* camera = renderer->GetActiveCamera();
    if (camera == nullptr) {
        return;
    }

    camera->ParallelProjectionOn();
    camera->SetFocalPoint(center);

    // AVL SCAN volumes: sagittal <- Z slices, coronal <- Y slices, axial <- X slices.
    // Camera positions place the observer on the positive axis side of each plane.
    // ViewUp is chosen so the final image orientation matches anatomical convention:
    //   Sagittal (camera +Z, plane XY): ViewUp=-X gives the correct 90° CW alignment.
    //   Coronal  (camera -Y, plane XZ): ViewUp=-X gives the correct 180° alignment.
    //   Axial    (camera +X, plane YZ): ViewUp=-Y gives the correct 90° CCW alignment.
    if (plane_code == 'S') {
        camera->SetPosition(center[0], center[1], center[2] + distance);
        camera->SetViewUp(-1.0, 0.0, 0.0);
        camera->SetParallelScale(0.5 * std::max(size_x, size_y));
    } else if (plane_code == 'C') {
        camera->SetPosition(center[0], center[1] - distance, center[2]);
        camera->SetViewUp(-1.0, 0.0, 0.0);
        camera->SetParallelScale(0.5 * std::max(size_x, size_z));
    } else {
        camera->SetPosition(center[0] + distance, center[1], center[2]);
        camera->SetViewUp(0.0, -1.0, 0.0);
        camera->SetParallelScale(0.5 * std::max(size_y, size_z));
    }

    camera->OrthogonalizeViewUp();
    camera->SetClippingRange(0.1, distance * 4.0);
    renderer->ResetCameraClippingRange(bounds);
}

void ComputeRobustWindowLevel(
    vtkImageData* image,
    double fallback_range[2],
    double* out_window,
    double* out_level) {
    if (image == nullptr || out_window == nullptr || out_level == nullptr) {
        return;
    }

    vtkDataArray* scalars = image->GetPointData() ? image->GetPointData()->GetScalars() : nullptr;
    if (scalars == nullptr || scalars->GetNumberOfTuples() <= 0) {
        const double fallback_window = std::max(1.0, fallback_range[1] - fallback_range[0]);
        *out_window = fallback_window;
        *out_level = fallback_range[0] + 0.5 * fallback_window;
        return;
    }

    const vtkIdType total = scalars->GetNumberOfTuples();
    const vtkIdType target_samples = std::min<vtkIdType>(50000, total);
    const vtkIdType stride = std::max<vtkIdType>(1, total / std::max<vtkIdType>(1, target_samples));

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(target_samples));
    for (vtkIdType i = 0; i < total; i += stride) {
        samples.push_back(scalars->GetComponent(i, 0));
    }

    if (samples.empty()) {
        const double fallback_window = std::max(1.0, fallback_range[1] - fallback_range[0]);
        *out_window = fallback_window;
        *out_level = fallback_range[0] + 0.5 * fallback_window;
        return;
    }

    std::sort(samples.begin(), samples.end());
    const size_t n = samples.size();
    const size_t low_index = static_cast<size_t>(0.01 * static_cast<double>(n - 1));
    const size_t high_index = static_cast<size_t>(0.99 * static_cast<double>(n - 1));
    const double low = samples[low_index];
    const double high = samples[high_index];

    if (high > low) {
        *out_window = std::max(1.0, high - low);
        *out_level = low + 0.5 * (*out_window);
        return;
    }

    const double fallback_window = std::max(1.0, fallback_range[1] - fallback_range[0]);
    *out_window = fallback_window;
    *out_level = fallback_range[0] + 0.5 * fallback_window;
}

class TriPlanarInteractorStyle : public vtkInteractorStyleImage {
public:
    static TriPlanarInteractorStyle* New();
    vtkTypeMacro(TriPlanarInteractorStyle, vtkInteractorStyleImage);

    void Configure(
        vtkRenderer* sagittal_renderer,
        vtkImageSliceMapper* sagittal_mapper,
        int sagittal_min,
        int sagittal_max,
        vtkRenderer* coronal_renderer,
        vtkImageSliceMapper* coronal_mapper,
        int coronal_min,
        int coronal_max,
        vtkRenderer* axial_renderer,
        vtkImageSliceMapper* axial_mapper,
        int axial_min,
        int axial_max) {
        this->SagittalRenderer = sagittal_renderer;
        this->SagittalMapper = sagittal_mapper;
        this->SagittalMin = sagittal_min;
        this->SagittalMax = sagittal_max;

        this->CoronalRenderer = coronal_renderer;
        this->CoronalMapper = coronal_mapper;
        this->CoronalMin = coronal_min;
        this->CoronalMax = coronal_max;

        this->AxialRenderer = axial_renderer;
        this->AxialMapper = axial_mapper;
        this->AxialMin = axial_min;
        this->AxialMax = axial_max;
    }

    void ConfigureInfoText(
        vtkRenderer* info_renderer,
        vtkTextActor* info_actor,
        const std::string& source_directory,
        int dim_x,
        int dim_y,
        int dim_z,
        double spacing_x,
        double spacing_y,
        double spacing_z,
        double scalar_min,
        double scalar_max) {
        this->InfoRenderer = info_renderer;
        this->InfoActor = info_actor;
        this->SourceDirectory = source_directory;
        this->DimX = dim_x;
        this->DimY = dim_y;
        this->DimZ = dim_z;
        this->SpacingX = spacing_x;
        this->SpacingY = spacing_y;
        this->SpacingZ = spacing_z;
        this->ScalarMin = scalar_min;
        this->ScalarMax = scalar_max;
        this->UpdateInfoText();
    }

    void SetCurrentSlices(int sagittal, int coronal, int axial) {
        this->SagittalSlice = sagittal;
        this->CoronalSlice = coronal;
        this->AxialSlice = axial;
        this->UpdateInfoText();
    }

    void OnMouseWheelForward() override {
        this->ScrollSlice(+1);
    }

    void OnMouseWheelBackward() override {
        this->ScrollSlice(-1);
    }

private:
    void ScrollSlice(int delta) {
        if (this->Interactor == nullptr) {
            return;
        }

        const int* pos = this->Interactor->GetEventPosition();
        vtkRenderer* poked = this->Interactor->FindPokedRenderer(pos[0], pos[1]);

        if (poked == this->SagittalRenderer && this->SagittalMapper != nullptr) {
            this->SagittalSlice = std::max(this->SagittalMin, std::min(this->SagittalMax, this->SagittalSlice + delta));
            this->SagittalMapper->SetSliceNumber(this->SagittalSlice);
        } else if (poked == this->CoronalRenderer && this->CoronalMapper != nullptr) {
            this->CoronalSlice = std::max(this->CoronalMin, std::min(this->CoronalMax, this->CoronalSlice + delta));
            this->CoronalMapper->SetSliceNumber(this->CoronalSlice);
        } else if (poked == this->AxialRenderer && this->AxialMapper != nullptr) {
            this->AxialSlice = std::max(this->AxialMin, std::min(this->AxialMax, this->AxialSlice + delta));
            this->AxialMapper->SetSliceNumber(this->AxialSlice);
        } else if (poked == this->InfoRenderer) {
            // Keep info pane passive (no slice scrolling).
            return;
        } else {
            // If mouse is outside all panes, fallback to axial scrolling.
            if (this->AxialMapper == nullptr) {
                return;
            }
            this->AxialSlice = std::max(this->AxialMin, std::min(this->AxialMax, this->AxialSlice + delta));
            this->AxialMapper->SetSliceNumber(this->AxialSlice);
        }

        this->UpdateInfoText();
        this->Interactor->Render();
    }

    void UpdateInfoText() {
        if (this->InfoActor == nullptr) {
            return;
        }

        std::ostringstream oss;
        oss << "DICOM KEY INFO\n\n"
            << "Source Folder:\n" << this->SourceDirectory << "\n\n"
            << "Volume Dimensions (X,Y,Z):\n"
            << this->DimX << " x " << this->DimY << " x " << this->DimZ << "\n\n"
            << "Voxel Spacing (mm):\n"
            << std::fixed << std::setprecision(3)
            << this->SpacingX << ", " << this->SpacingY << ", " << this->SpacingZ << "\n\n"
            << "Scalar Range:\n"
            << std::setprecision(1)
            << this->ScalarMin << " to " << this->ScalarMax << "\n\n"
            << "Current Slices:\n"
            << "Sagittal (X): " << (this->SagittalSlice + 1) << " / " << std::max(1, this->DimX) << "\n"
            << "Coronal  (Y): " << (this->CoronalSlice + 1) << " / " << std::max(1, this->DimY) << "\n"
            << "Axial    (Z): " << (this->AxialSlice + 1) << " / " << std::max(1, this->DimZ) << "\n\n"
            << "Interaction:\n"
            << "Mouse wheel over each pane to move its slice.";

        this->InfoActor->SetInput(oss.str().c_str());
    }

    vtkRenderer* SagittalRenderer = nullptr;
    vtkImageSliceMapper* SagittalMapper = nullptr;
    int SagittalSlice = 0;
    int SagittalMin = 0;
    int SagittalMax = 0;

    vtkRenderer* CoronalRenderer = nullptr;
    vtkImageSliceMapper* CoronalMapper = nullptr;
    int CoronalSlice = 0;
    int CoronalMin = 0;
    int CoronalMax = 0;

    vtkRenderer* AxialRenderer = nullptr;
    vtkImageSliceMapper* AxialMapper = nullptr;
    int AxialSlice = 0;
    int AxialMin = 0;
    int AxialMax = 0;

    vtkRenderer* InfoRenderer = nullptr;
    vtkTextActor* InfoActor = nullptr;

    std::string SourceDirectory;
    int DimX = 0;
    int DimY = 0;
    int DimZ = 0;
    double SpacingX = 0.0;
    double SpacingY = 0.0;
    double SpacingZ = 0.0;
    double ScalarMin = 0.0;
    double ScalarMax = 0.0;
};

vtkStandardNewMacro(TriPlanarInteractorStyle);

}  // namespace

// Returns true if the directory contains at least one file that looks like a
// DICOM image slice (.dcm / .dicom, or extension-less files that are NOT the
// well-known DICOMDIR index).
static bool DirectoryLooksLikeDicom(const std::string& directory) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const auto ext  = entry.path().extension().string();
        const auto stem = entry.path().stem().string();
        std::string lower_ext = ext;
        std::transform(lower_ext.begin(), lower_ext.end(), lower_ext.begin(), ::tolower);
        std::string lower_stem = stem;
        std::transform(lower_stem.begin(), lower_stem.end(), lower_stem.begin(), ::tolower);

        if (lower_ext == ".dcm" || lower_ext == ".dicom") return true;
        // Extension-less slices (e.g. IM000001), but skip DICOMDIR / DICOM marker
        if (lower_ext.empty() && lower_stem != "dicomdir" && lower_stem != "dicom")
            return true;
    }
    return false;
}

bool ValidateDicomDirectory(const std::string& directory, std::string& error_message) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        error_message = "Path is not a valid directory: " + directory;
        return false;
    }
    if (!DirectoryLooksLikeDicom(directory)) {
        error_message =
            "The selected folder does not appear to contain a DICOM series.\n"
            "Please select a folder that contains .dcm files or extension-less "
            "DICOM files.\n\nSelected: " + directory;
        return false;
    }

    return true;
}

bool ShowDicomVolume(const std::string& directory, std::string& error_message) {
    // ── Validate directory ──────────────────────────────────────────────────
    if (!ValidateDicomDirectory(directory, error_message)) {
        return false;
    }

    // Normalize path separators for VTK (use forward slashes)
    std::string dir_path = directory;
    for (auto& ch : dir_path) {
        if (ch == '\\') ch = '/';
    }

    // ── Load DICOM ──────────────────────────────────────────────────────────
    vtkNew<vtkDICOMImageReader> reader;
    vtkSmartPointer<vtkImageData> fallback_image;
    bool use_direct_input = false;
    try {
        reader->SetDirectoryName(dir_path.c_str());
        reader->Update();
    } catch (const std::exception& ex) {
#ifdef FDK_HAS_DCMTK
        fallback_image = LoadDicomVolumeWithDcmtk(directory, error_message);
        if (fallback_image == nullptr) {
            error_message = std::string("VTK DICOM reader error: ") + ex.what() + "\n" + error_message;
            return false;
        }
        use_direct_input = true;
#else
        error_message = std::string("VTK DICOM reader error: ") + ex.what();
        return false;
#endif
    } catch (...) {
#ifdef FDK_HAS_DCMTK
        fallback_image = LoadDicomVolumeWithDcmtk(directory, error_message);
        if (fallback_image == nullptr) {
            if (error_message.empty()) {
                error_message = "VTK DICOM reader encountered an unknown error.";
            }
            return false;
        }
        use_direct_input = true;
#else
        error_message = "VTK DICOM reader encountered an unknown error.";
        return false;
#endif
    }

    vtkImageData* image = use_direct_input ? fallback_image.GetPointer() : reader->GetOutput();
    if (image == nullptr) {
        error_message = "vtkDICOMImageReader returned no data for: " + directory;
        return false;
    }

    int dims[3] = {0, 0, 0};
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
#ifdef FDK_HAS_DCMTK
        fallback_image = LoadDicomVolumeWithDcmtk(directory, error_message);
        if (fallback_image != nullptr) {
            use_direct_input = true;
            image = fallback_image.GetPointer();
            image->GetDimensions(dims);
        }
#endif
    }
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        error_message =
            "DICOM volume has invalid dimensions (" +
            std::to_string(dims[0]) + " x " +
            std::to_string(dims[1]) + " x " +
            std::to_string(dims[2]) + ").\n"
            "The folder may contain an incomplete series.";
        return false;
    }

    double spacing[3] = {0.0, 0.0, 0.0};
    image->GetSpacing(spacing);
    double scalar_range[2] = {0.0, 0.0};
    image->GetScalarRange(scalar_range);

    // ── Tri-planar slice viewer ─────────────────────────────────────────────
    const int sagittal_slice = dims[0] / 2; // X: sagittal
    const int coronal_slice = dims[1] / 2;  // Y: coronal
    const int axial_slice = dims[2] / 2;    // Z: axial

    vtkNew<vtkImageSliceMapper> sagittal_mapper;
    if (use_direct_input) {
        sagittal_mapper->SetInputData(image);
    } else {
        sagittal_mapper->SetInputConnection(reader->GetOutputPort());
    }
    sagittal_mapper->SetOrientationToX();
    sagittal_mapper->SetSliceNumber(sagittal_slice);
    sagittal_mapper->BorderOn();

    vtkNew<vtkImageSliceMapper> coronal_mapper;
    if (use_direct_input) {
        coronal_mapper->SetInputData(image);
    } else {
        coronal_mapper->SetInputConnection(reader->GetOutputPort());
    }
    coronal_mapper->SetOrientationToY();
    coronal_mapper->SetSliceNumber(coronal_slice);
    coronal_mapper->BorderOn();

    vtkNew<vtkImageSliceMapper> axial_mapper;
    if (use_direct_input) {
        axial_mapper->SetInputData(image);
    } else {
        axial_mapper->SetInputConnection(reader->GetOutputPort());
    }
    axial_mapper->SetOrientationToZ();
    axial_mapper->SetSliceNumber(axial_slice);
    axial_mapper->BorderOn();

    vtkNew<vtkImageSlice> sagittal_actor;
    sagittal_actor->SetMapper(sagittal_mapper);
    sagittal_actor->GetProperty()->SetColorWindow(400.0);
    sagittal_actor->GetProperty()->SetColorLevel(40.0);

    vtkNew<vtkImageSlice> coronal_actor;
    coronal_actor->SetMapper(coronal_mapper);
    coronal_actor->GetProperty()->SetColorWindow(400.0);
    coronal_actor->GetProperty()->SetColorLevel(40.0);

    vtkNew<vtkImageSlice> axial_actor;
    axial_actor->SetMapper(axial_mapper);
    axial_actor->GetProperty()->SetColorWindow(400.0);
    axial_actor->GetProperty()->SetColorLevel(40.0);

    vtkNew<vtkRenderer> sagittal_renderer;
    sagittal_renderer->SetViewport(0.02, 0.52, 0.48, 0.98);
    sagittal_renderer->SetBackground(0.08, 0.08, 0.08);
    sagittal_renderer->AddActor(sagittal_actor);
    ConfigureSliceCamera(sagittal_renderer, sagittal_actor, 0);

    vtkNew<vtkCornerAnnotation> sagittal_label;
    sagittal_label->SetText(2, "SAGITTAL");
    sagittal_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    sagittal_label->GetTextProperty()->SetFontSize(16);
    sagittal_renderer->AddViewProp(sagittal_label);
    AddPatientPlaneGlyph(sagittal_renderer, 'S');

    vtkNew<vtkRenderer> coronal_renderer;
    coronal_renderer->SetViewport(0.52, 0.52, 0.98, 0.98);
    coronal_renderer->SetBackground(0.08, 0.08, 0.08);
    coronal_renderer->AddActor(coronal_actor);
    ConfigureSliceCamera(coronal_renderer, coronal_actor, 1);

    vtkNew<vtkCornerAnnotation> coronal_label;
    coronal_label->SetText(2, "CORONAL");
    coronal_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    coronal_label->GetTextProperty()->SetFontSize(16);
    coronal_renderer->AddViewProp(coronal_label);
    AddPatientPlaneGlyph(coronal_renderer, 'C');

    vtkNew<vtkRenderer> axial_renderer;
    axial_renderer->SetViewport(0.02, 0.02, 0.48, 0.48);
    axial_renderer->SetBackground(0.08, 0.08, 0.08);
    axial_renderer->AddActor(axial_actor);
    ConfigureSliceCamera(axial_renderer, axial_actor, 2);

    vtkNew<vtkCornerAnnotation> axial_label;
    axial_label->SetText(2, "AXIAL");
    axial_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    axial_label->GetTextProperty()->SetFontSize(16);
    axial_renderer->AddViewProp(axial_label);
    AddPatientPlaneGlyph(axial_renderer, 'A');

    vtkNew<vtkRenderer> info_renderer;
    info_renderer->SetViewport(0.52, 0.02, 0.98, 0.48);
    info_renderer->SetBackground(0.10, 0.12, 0.16);

    vtkNew<vtkTextActor> info_text;
    info_text->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    info_text->SetPosition(0.03, 0.95);
    info_text->GetTextProperty()->SetJustificationToLeft();
    info_text->GetTextProperty()->SetVerticalJustificationToTop();
    info_text->GetTextProperty()->SetColor(0.88, 0.92, 0.98);
    info_text->GetTextProperty()->SetFontSize(18);
    info_text->GetTextProperty()->SetLineSpacing(1.2);
    info_renderer->AddActor2D(info_text);

    vtkNew<vtkRenderWindow> render_window;
    render_window->SetWindowName("DICOM Multi-Planar Viewer");
    render_window->SetSize(1400, 1100);
    render_window->SetBorders(1);
    render_window->SetMultiSamples(0);
    render_window->AddRenderer(sagittal_renderer);
    render_window->AddRenderer(coronal_renderer);
    render_window->AddRenderer(axial_renderer);
    render_window->AddRenderer(info_renderer);

    vtkNew<vtkRenderWindowInteractor> interactor;
    vtkNew<TriPlanarInteractorStyle> style;
    style->Configure(
        sagittal_renderer, sagittal_mapper, 0, std::max(0, dims[0] - 1),
        coronal_renderer, coronal_mapper, 0, std::max(0, dims[1] - 1),
        axial_renderer, axial_mapper, 0, std::max(0, dims[2] - 1));
    style->ConfigureInfoText(
        info_renderer,
        info_text,
        directory,
        dims[0],
        dims[1],
        dims[2],
        spacing[0],
        spacing[1],
        spacing[2],
        scalar_range[0],
        scalar_range[1]);
    style->SetCurrentSlices(sagittal_slice, coronal_slice, axial_slice);

    interactor->SetInteractorStyle(style);
    interactor->SetRenderWindow(render_window);

    render_window->Render();
    interactor->Start();
    return true;
}

// ============================================================================
// ShowRawVolume – Display a pre-loaded vtkImageData volume (e.g., from raw file)
// ============================================================================
bool ShowRawVolume(const vtkSmartPointer<vtkImageData>& image,
                  const std::string& title,
                  std::string& error_message) {
    if (image == nullptr) {
        error_message = "Input image is null";
        return false;
    }

    int dims[3] = {0, 0, 0};
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        error_message = "Image has invalid dimensions";
        return false;
    }

    vtkSmartPointer<vtkImageData> oriented_image = image;

    int new_dims[3] = {0, 0, 0};
    oriented_image->GetDimensions(new_dims);

    double spacing[3] = {0.0, 0.0, 0.0};
    oriented_image->GetSpacing(spacing);
    double scalar_range[2] = {0.0, 0.0};
    oriented_image->GetScalarRange(scalar_range);

    // Use robust window/level calculation to avoid extreme values affecting contrast
    double color_window = 1.0;
    double color_level = 0.0;
    ComputeRobustWindowLevel(oriented_image, scalar_range, &color_window, &color_level);

    // ── Tri-planar slice viewer ─────────────────────────────────────────────
    const int sagittal_slice = new_dims[0] / 2; // X: sagittal
    const int coronal_slice = new_dims[2] / 2;  // Z: coronal (A/P axis)
    const int axial_slice = new_dims[1] / 2;    // Y: axial (S/I axis)

    vtkNew<vtkImageSliceMapper> sagittal_mapper;
    sagittal_mapper->SetInputData(oriented_image);
    sagittal_mapper->SetOrientationToX();
    sagittal_mapper->SetSliceNumber(sagittal_slice);
    sagittal_mapper->BorderOn();

    vtkNew<vtkImageSliceMapper> coronal_mapper;
    coronal_mapper->SetInputData(oriented_image);
    coronal_mapper->SetOrientationToZ();
    coronal_mapper->SetSliceNumber(coronal_slice);
    coronal_mapper->BorderOn();

    vtkNew<vtkImageSliceMapper> axial_mapper;
    axial_mapper->SetInputData(oriented_image);
    axial_mapper->SetOrientationToY();
    axial_mapper->SetSliceNumber(axial_slice);
    axial_mapper->BorderOn();

    vtkNew<vtkImageSlice> sagittal_actor;
    sagittal_actor->SetMapper(sagittal_mapper);
    sagittal_actor->GetProperty()->SetColorWindow(color_window);
    sagittal_actor->GetProperty()->SetColorLevel(color_level);

    vtkNew<vtkImageSlice> coronal_actor;
    coronal_actor->SetMapper(coronal_mapper);
    coronal_actor->GetProperty()->SetColorWindow(color_window);
    coronal_actor->GetProperty()->SetColorLevel(color_level);

    vtkNew<vtkImageSlice> axial_actor;
    axial_actor->SetMapper(axial_mapper);
    axial_actor->GetProperty()->SetColorWindow(color_window);
    axial_actor->GetProperty()->SetColorLevel(color_level);

    vtkNew<vtkRenderer> sagittal_renderer;
    sagittal_renderer->SetViewport(0.02, 0.52, 0.48, 0.98);
    sagittal_renderer->SetBackground(0.08, 0.08, 0.08);
    sagittal_renderer->AddActor(sagittal_actor);
    ConfigureRawSliceCamera(sagittal_renderer, sagittal_actor, 'S');

    vtkNew<vtkCornerAnnotation> sagittal_label;
    sagittal_label->SetText(2, "SAGITTAL");
    sagittal_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    sagittal_label->GetTextProperty()->SetFontSize(16);
    sagittal_renderer->AddViewProp(sagittal_label);
    AddPatientPlaneGlyph(sagittal_renderer, 'S');

    vtkNew<vtkRenderer> coronal_renderer;
    coronal_renderer->SetViewport(0.52, 0.52, 0.98, 0.98);
    coronal_renderer->SetBackground(0.08, 0.08, 0.08);
    coronal_renderer->AddActor(coronal_actor);
    ConfigureRawSliceCamera(coronal_renderer, coronal_actor, 'C');

    vtkNew<vtkCornerAnnotation> coronal_label;
    coronal_label->SetText(2, "CORONAL");
    coronal_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    coronal_label->GetTextProperty()->SetFontSize(16);
    coronal_renderer->AddViewProp(coronal_label);
    AddPatientPlaneGlyph(coronal_renderer, 'C');

    vtkNew<vtkRenderer> axial_renderer;
    axial_renderer->SetViewport(0.02, 0.02, 0.48, 0.48);
    axial_renderer->SetBackground(0.08, 0.08, 0.08);
    axial_renderer->AddActor(axial_actor);
    ConfigureRawSliceCamera(axial_renderer, axial_actor, 'A');

    vtkNew<vtkCornerAnnotation> axial_label;
    axial_label->SetText(2, "AXIAL");
    axial_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    axial_label->GetTextProperty()->SetFontSize(16);
    axial_renderer->AddViewProp(axial_label);
    AddPatientPlaneGlyph(axial_renderer, 'A');

    vtkNew<vtkRenderer> info_renderer;
    info_renderer->SetViewport(0.52, 0.02, 0.98, 0.48);
    info_renderer->SetBackground(0.10, 0.12, 0.16);

    vtkNew<vtkTextActor> info_text;
    info_text->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    info_text->SetPosition(0.03, 0.95);
    info_text->GetTextProperty()->SetJustificationToLeft();
    info_text->GetTextProperty()->SetVerticalJustificationToTop();
    info_text->GetTextProperty()->SetColor(0.88, 0.92, 0.98);
    info_text->GetTextProperty()->SetFontSize(18);
    info_text->GetTextProperty()->SetLineSpacing(1.2);
    info_renderer->AddActor2D(info_text);

    vtkNew<vtkRenderWindow> render_window;
    render_window->SetWindowName(title.c_str());
    render_window->SetSize(1400, 1100);
    render_window->SetBorders(1);
    render_window->SetMultiSamples(0);
    render_window->AddRenderer(sagittal_renderer);
    render_window->AddRenderer(coronal_renderer);
    render_window->AddRenderer(axial_renderer);
    render_window->AddRenderer(info_renderer);

    vtkNew<vtkRenderWindowInteractor> interactor;
    vtkNew<TriPlanarInteractorStyle> style;
    style->Configure(
        sagittal_renderer, sagittal_mapper, 0, std::max(0, new_dims[0] - 1),
        coronal_renderer, coronal_mapper, 0, std::max(0, new_dims[2] - 1),
        axial_renderer, axial_mapper, 0, std::max(0, new_dims[1] - 1));
    
    // Format info text for raw volume
    std::ostringstream oss;
    oss << "Raw Volume\n"
        << "Dims: " << new_dims[0] << " x " << new_dims[1] << " x " << new_dims[2] << "\n"
        << "Spacing: " << spacing[0] << " x " << spacing[1] << " x " << spacing[2] << " cm\n"
        << "Range: [" << scalar_range[0] << ", " << scalar_range[1] << "]";
    
    style->ConfigureInfoText(
        info_renderer,
        info_text,
        oss.str(),
        new_dims[0],
        new_dims[1],
        new_dims[2],
        spacing[0],
        spacing[1],
        spacing[2],
        scalar_range[0],
        scalar_range[1]);
    style->SetCurrentSlices(sagittal_slice, coronal_slice, axial_slice);

    interactor->SetInteractorStyle(style);
    interactor->SetRenderWindow(render_window);

    render_window->Render();
    interactor->Start();
    return true;
}

bool ShowScanVolume(const vtkSmartPointer<vtkImageData>& image,
                  const std::string& title,
                  std::string& error_message) {
    if (image == nullptr) {
        error_message = "Input image is null";
        return false;
    }

    int dims[3] = {0, 0, 0};
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        error_message = "Image has invalid dimensions";
        return false;
    }

    double spacing[3] = {0.0, 0.0, 0.0};
    image->GetSpacing(spacing);
    double scalar_range[2] = {0.0, 0.0};
    image->GetScalarRange(scalar_range);

    double color_window = 1.0;
    double color_level = 0.0;
    ComputeRobustWindowLevel(image, scalar_range, &color_window, &color_level);

    // AVL SCAN datasets use a different anatomical-axis mapping than the raw path.
    const int sagittal_slice = dims[2] / 2; // Z -> sagittal
    const int coronal_slice = dims[1] / 2;  // Y -> coronal
    const int axial_slice = dims[0] / 2;    // X -> axial

    vtkNew<vtkImageSliceMapper> sagittal_mapper;
    sagittal_mapper->SetInputData(image);
    sagittal_mapper->SetOrientationToZ();
    sagittal_mapper->SetSliceNumber(sagittal_slice);
    sagittal_mapper->BorderOn();

    vtkNew<vtkImageSliceMapper> coronal_mapper;
    coronal_mapper->SetInputData(image);
    coronal_mapper->SetOrientationToY();
    coronal_mapper->SetSliceNumber(coronal_slice);
    coronal_mapper->BorderOn();

    vtkNew<vtkImageSliceMapper> axial_mapper;
    axial_mapper->SetInputData(image);
    axial_mapper->SetOrientationToX();
    axial_mapper->SetSliceNumber(axial_slice);
    axial_mapper->BorderOn();

    vtkNew<vtkImageSlice> sagittal_actor;
    sagittal_actor->SetMapper(sagittal_mapper);
    sagittal_actor->GetProperty()->SetColorWindow(color_window);
    sagittal_actor->GetProperty()->SetColorLevel(color_level);

    vtkNew<vtkImageSlice> coronal_actor;
    coronal_actor->SetMapper(coronal_mapper);
    coronal_actor->GetProperty()->SetColorWindow(color_window);
    coronal_actor->GetProperty()->SetColorLevel(color_level);

    vtkNew<vtkImageSlice> axial_actor;
    axial_actor->SetMapper(axial_mapper);
    axial_actor->GetProperty()->SetColorWindow(color_window);
    axial_actor->GetProperty()->SetColorLevel(color_level);

    vtkNew<vtkRenderer> sagittal_renderer;
    sagittal_renderer->SetViewport(0.02, 0.52, 0.48, 0.98);
    sagittal_renderer->SetBackground(0.08, 0.08, 0.08);
    sagittal_renderer->AddActor(sagittal_actor);
    ConfigureScanSliceCamera(sagittal_renderer, sagittal_actor, 'S');

    vtkNew<vtkCornerAnnotation> sagittal_label;
    sagittal_label->SetText(2, "SAGITTAL");
    sagittal_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    sagittal_label->GetTextProperty()->SetFontSize(16);
    sagittal_renderer->AddViewProp(sagittal_label);
    AddPatientPlaneGlyph(sagittal_renderer, 'S');

    vtkNew<vtkRenderer> coronal_renderer;
    coronal_renderer->SetViewport(0.52, 0.52, 0.98, 0.98);
    coronal_renderer->SetBackground(0.08, 0.08, 0.08);
    coronal_renderer->AddActor(coronal_actor);
    ConfigureScanSliceCamera(coronal_renderer, coronal_actor, 'C');

    vtkNew<vtkCornerAnnotation> coronal_label;
    coronal_label->SetText(2, "CORONAL");
    coronal_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    coronal_label->GetTextProperty()->SetFontSize(16);
    coronal_renderer->AddViewProp(coronal_label);
    AddPatientPlaneGlyph(coronal_renderer, 'C');

    vtkNew<vtkRenderer> axial_renderer;
    axial_renderer->SetViewport(0.02, 0.02, 0.48, 0.48);
    axial_renderer->SetBackground(0.08, 0.08, 0.08);
    axial_renderer->AddActor(axial_actor);
    ConfigureScanSliceCamera(axial_renderer, axial_actor, 'A');

    vtkNew<vtkCornerAnnotation> axial_label;
    axial_label->SetText(2, "AXIAL");
    axial_label->GetTextProperty()->SetColor(0.95, 0.95, 0.95);
    axial_label->GetTextProperty()->SetFontSize(16);
    axial_renderer->AddViewProp(axial_label);
    AddPatientPlaneGlyph(axial_renderer, 'A');

    vtkNew<vtkRenderer> info_renderer;
    info_renderer->SetViewport(0.52, 0.02, 0.98, 0.48);
    info_renderer->SetBackground(0.10, 0.12, 0.16);

    vtkNew<vtkTextActor> info_text;
    info_text->GetPositionCoordinate()->SetCoordinateSystemToNormalizedViewport();
    info_text->SetPosition(0.03, 0.95);
    info_text->GetTextProperty()->SetJustificationToLeft();
    info_text->GetTextProperty()->SetVerticalJustificationToTop();
    info_text->GetTextProperty()->SetColor(0.88, 0.92, 0.98);
    info_text->GetTextProperty()->SetFontSize(18);
    info_text->GetTextProperty()->SetLineSpacing(1.2);
    info_renderer->AddActor2D(info_text);

    vtkNew<vtkRenderWindow> render_window;
    render_window->SetWindowName(title.c_str());
    render_window->SetSize(1400, 1100);
    render_window->SetBorders(1);
    render_window->SetMultiSamples(0);
    render_window->AddRenderer(sagittal_renderer);
    render_window->AddRenderer(coronal_renderer);
    render_window->AddRenderer(axial_renderer);
    render_window->AddRenderer(info_renderer);

    vtkNew<vtkRenderWindowInteractor> interactor;
    vtkNew<TriPlanarInteractorStyle> style;
    style->Configure(
        sagittal_renderer, sagittal_mapper, 0, std::max(0, dims[2] - 1),
        coronal_renderer, coronal_mapper, 0, std::max(0, dims[1] - 1),
        axial_renderer, axial_mapper, 0, std::max(0, dims[0] - 1));

    std::ostringstream oss;
    oss << "Scan Volume\n"
        << "Dims: " << dims[0] << " x " << dims[1] << " x " << dims[2] << "\n"
        << "Spacing: " << spacing[0] << " x " << spacing[1] << " x " << spacing[2] << "\n"
        << "Range: [" << scalar_range[0] << ", " << scalar_range[1] << "]\n"
        << "WL: " << color_level << " / WW: " << color_window;

    style->ConfigureInfoText(
        info_renderer,
        info_text,
        oss.str(),
        dims[0],
        dims[1],
        dims[2],
        spacing[0],
        spacing[1],
        spacing[2],
        scalar_range[0],
        scalar_range[1]);
    style->SetCurrentSlices(sagittal_slice, coronal_slice, axial_slice);

    interactor->SetInteractorStyle(style);
    interactor->SetRenderWindow(render_window);

    render_window->Render();
    interactor->Start();
    return true;
}

}  // namespace dicom_viewer
