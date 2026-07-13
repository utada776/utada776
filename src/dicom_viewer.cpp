#include "dicom_viewer.h"

// ── VTK factory registration ────────────────────────────────────────────────
// Force registration explicitly (cmake-generated autoinit may fail on paths
// containing spaces under MSVC).
#include <vtkAutoInit.h>
VTK_MODULE_INIT(vtkRenderingOpenGL2)
VTK_MODULE_INIT(vtkInteractionStyle)
// ────────────────────────────────────────────────────────────────────────────

#include <vtkDICOMImageReader.h>
#include <vtkImageData.h>
#include <vtkImageProperty.h>
#include <vtkImageSlice.h>
#include <vtkImageSliceMapper.h>
#include <vtkInteractorStyleImage.h>
#include <vtkNew.h>
#include <vtkRenderWindow.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkRenderer.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace dicom_viewer {

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

bool ShowDicomVolume(const std::string& directory, std::string& error_message) {
    // ── Validate directory ──────────────────────────────────────────────────
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

    // ── Load DICOM ──────────────────────────────────────────────────────────
    vtkNew<vtkDICOMImageReader> reader;
    reader->SetDirectoryName(directory.c_str());
    reader->Update();

    vtkImageData* image = reader->GetOutput();
    if (image == nullptr) {
        error_message = "vtkDICOMImageReader returned no data for: " + directory;
        return false;
    }

    int dims[3] = {0, 0, 0};
    image->GetDimensions(dims);
    if (dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        error_message =
            "DICOM volume has invalid dimensions (" +
            std::to_string(dims[0]) + " x " +
            std::to_string(dims[1]) + " x " +
            std::to_string(dims[2]) + ").\n"
            "The folder may contain an incomplete series.";
        return false;
    }

    // ── Slice viewer (axial) ────────────────────────────────────────────────
    // Start in the middle slice
    const int mid_slice = dims[2] / 2;

    vtkNew<vtkImageSliceMapper> slice_mapper;
    slice_mapper->SetInputConnection(reader->GetOutputPort());
    slice_mapper->SetOrientationToZ();   // axial view
    slice_mapper->SetSliceNumber(mid_slice);
    slice_mapper->BorderOn();

    vtkNew<vtkImageSlice> slice_actor;
    slice_actor->SetMapper(slice_mapper);
    slice_actor->GetProperty()->SetColorWindow(400.0);
    slice_actor->GetProperty()->SetColorLevel(40.0);

    vtkNew<vtkRenderer> renderer;
    renderer->SetBackground(0.05, 0.05, 0.05);
    renderer->AddActor(slice_actor);
    renderer->ResetCamera();

    vtkNew<vtkRenderWindow> render_window;
    render_window->SetWindowName("DICOM Slice Viewer");
    render_window->SetSize(1024, 1024);
    render_window->AddRenderer(renderer);

    vtkNew<vtkRenderWindowInteractor> interactor;
    vtkNew<vtkInteractorStyleImage> style;
    interactor->SetInteractorStyle(style);
    interactor->SetRenderWindow(render_window);

    render_window->Render();
    interactor->Start();
    return true;
}

}  // namespace dicom_viewer
