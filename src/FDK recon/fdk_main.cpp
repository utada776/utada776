// ============================================================================
// fdk_main.cpp
//
// GUI implementation for the FDK 3D reconstruction workflow.
//
// Two windows:
//  1) FdkStartupDialog  – parameter input (mirrors XVI startup dialog)
//  2) FdkReviewFrame    – online reconstruction viewer (4-panel layout)
//     Top-left:    Transverse (axial) slice
//     Top-right:   Coronal slice
//     Bottom-left: Sagittal slice
//     Bottom-right: Session log
//
// Modelled after the Registration startup / review window pair in
// src/registration/registration_main.cpp.
// ============================================================================

#include "FDK recon/fdk_ui.h"
#include "FDK recon/fdk_recon.h"
#include "FDK recon/fdk_types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/datetime.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/wx.h>

#include "dicom/dicom_exporter.h"

namespace {

// ---------------------------------------------------------------------------
// Custom events for thread -> GUI communication
// ---------------------------------------------------------------------------
wxDECLARE_EVENT(wxEVT_FDK_LOG,      wxCommandEvent);
wxDECLARE_EVENT(wxEVT_FDK_PROGRESS, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_FDK_COMPLETE, wxCommandEvent);
wxDECLARE_EVENT(wxEVT_FDK_ERROR,    wxCommandEvent);

wxDEFINE_EVENT(wxEVT_FDK_LOG,      wxCommandEvent);
wxDEFINE_EVENT(wxEVT_FDK_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_FDK_COMPLETE, wxCommandEvent);
wxDEFINE_EVENT(wxEVT_FDK_ERROR,    wxCommandEvent);

// Last-used params (persist within the session)
static fdk::FdkParams s_last_params;
static bool           s_has_last_params = false;

// ---------------------------------------------------------------------------
// Helper: current timestamp string HH:MM:SS
// ---------------------------------------------------------------------------
static wxString NowStr() {
    return wxDateTime::Now().Format(wxT("%H:%M:%S"));
}

enum class PatientPlane {
    Sagittal,
    Coronal,
    Axial
};

static const char* PatientPlaneAssetName(PatientPlane plane) {
    if (plane == PatientPlane::Sagittal) {
        return "sagittal.png";
    }
    if (plane == PatientPlane::Coronal) {
        return "coronal.png";
    }
    return "axial.png";
}

static wxString FindPatientPlaneAsset(PatientPlane plane) {
    const wxString file_name = wxString::FromUTF8(PatientPlaneAssetName(plane));
    std::vector<wxString> candidates;
    candidates.push_back(wxGetCwd() + wxT("\\assets\\orientation\\") + file_name);
#ifdef HELLO_SOURCE_DIR
    candidates.push_back(wxString::FromUTF8(HELLO_SOURCE_DIR) + wxT("\\assets\\orientation\\") + file_name);
#endif
    for (const wxString& candidate : candidates) {
        if (wxFileExists(candidate)) {
            return candidate;
        }
    }
    return wxString();
}

static void DrawPatientPlaneGlyph(wxDC& dc, const wxSize& client_size, PatientPlane plane) {
    const int glyph_w = std::max(44, std::min(72, client_size.x / 6));
    const int glyph_h = glyph_w;
    const int margin = 12;
    const int left = client_size.x - glyph_w - margin;
    const int top = client_size.y - glyph_h - margin;
    if (left < margin || top < margin) {
        return;
    }

    const wxString asset_path = FindPatientPlaneAsset(plane);
    if (asset_path.IsEmpty()) {
        return;
    }
    wxImage icon;
    if (!icon.LoadFile(asset_path, wxBITMAP_TYPE_PNG) || !icon.IsOk()) {
        return;
    }
    wxBitmap bitmap(icon.Scale(glyph_w, glyph_h, wxIMAGE_QUALITY_HIGH));
    dc.DrawBitmap(bitmap, left, top, true);
}

static wxString FdkDefaultDicomDataRoot() {
    return wxT("C:\\code test\\dicom data");
}

static wxString FirstExistingDirectory(const std::vector<wxString>& candidates) {
    for (const wxString& candidate : candidates) {
        if (!candidate.IsEmpty() && wxDirExists(candidate)) {
            return candidate;
        }
    }
    return candidates.empty() ? wxString() : candidates.front();
}

static wxString FirstExistingFile(const std::vector<wxString>& candidates) {
    for (const wxString& candidate : candidates) {
        if (!candidate.IsEmpty() && wxFileExists(candidate)) {
            return candidate;
        }
    }
    return candidates.empty() ? wxString() : candidates.front();
}

static wxString DefaultFdkProjectionDir() {
    const wxString root = FdkDefaultDicomDataRoot();
    return FirstExistingDirectory({
        root + wxT("\\projections\\Head\\img_1.3.46.423632.14100020251119153130321.6"),
        root + wxT("\\projections\\Pelvis\\img_1.3.46.423632.141000202573115852350.34"),
        root + wxT("\\projections\\img_1.3.46.423632.14100020243296291654.15"),
        root + wxT("\\projections\\CAT\\img_1.3.46.423632.1410002026210153720947.9"),
        root + wxT("\\projections")
    });
}

static wxString DefaultFdkGainFile() {
    const wxString root = FdkDefaultDicomDataRoot();
    return FirstExistingFile({
        root + wxT("\\gain and flex map\\FLOOD_M_K120_F1_SLG.his"),
        root + wxT("\\gain and flex map\\FLOOD_M_K120_F0_SLG.his")
    });
}

static wxString DefaultFdkFrameInfoFile() {
    const wxString projection_dir = DefaultFdkProjectionDir();
    return FirstExistingFile({
        projection_dir + wxT("\\_Frames.xml"),
        FdkDefaultDicomDataRoot() + wxT("\\projections\\img_1.3.46.423632.14100020243296291654.15\\angle\\_Frames_generated_angles.txt")
    });
}

static wxString DefaultFdkIniFile() {
    const wxString projection_dir = DefaultFdkProjectionDir();
    return FirstExistingFile({
        projection_dir + wxT("\\Reconstruction\\Head.INI"),
        projection_dir + wxT("\\Reconstruction\\Pelvis.INI"),
        FdkDefaultDicomDataRoot() + wxT("\\demo_case_registration.ini")
    });
}

static wxString ExistingDirectoryOrFdkDataRoot(const wxString& preferred) {
    if (!preferred.IsEmpty() && wxDirExists(preferred)) {
        return preferred;
    }

    if (!preferred.IsEmpty()) {
        wxFileName file_name(preferred);
        const wxString parent = file_name.GetPath();
        if (!parent.IsEmpty() && wxDirExists(parent)) {
            return parent;
        }
    }

    const wxString root = FdkDefaultDicomDataRoot();
    if (wxDirExists(root)) {
        return root;
    }
    return wxGetCwd();
}

// ---------------------------------------------------------------------------
// SlicePanel – displays a greyscale 2D slice rendered via wxImage
// ---------------------------------------------------------------------------
class SlicePanel : public wxPanel {
public:
    SlicePanel(wxWindow* parent, const wxString& label, PatientPlane plane)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE)
        , m_label(label)
        , m_plane(plane) {
        SetBackgroundColour(wxColour(200, 200, 200));
        SetMinSize(wxSize(380, 340));
        Bind(wxEVT_PAINT, &SlicePanel::OnPaint, this);
        Bind(wxEVT_SIZE,  &SlicePanel::OnSize,  this);
        Bind(wxEVT_MOUSEWHEEL, &SlicePanel::OnMouseWheel, this);
        Bind(wxEVT_LEFT_DOWN,  &SlicePanel::OnLeftDown, this);
        Bind(wxEVT_LEFT_UP,    &SlicePanel::OnLeftUp, this);
        Bind(wxEVT_MOTION,     &SlicePanel::OnMouseMove, this);
    }

    // Update slice from volume (thread-safe via mutex)
    void SetSlice(const std::vector<unsigned char>& pixels, int w, int h,
                  const wxString& overlay_text) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pixels = pixels;
        m_img_w  = w;
        m_img_h  = h;
        m_overlay = overlay_text;
        m_dirty  = true;
    }

    void SetInteractionEnabled(bool enabled) {
        m_interactive_enabled = enabled;
    }

    void SetSliceWheelCallback(std::function<void(int)> cb) {
        m_on_slice_wheel = std::move(cb);
    }



    void RefreshSlice() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_dirty) return;
            m_dirty = false;
        }
        Refresh();
    }

private:
    void OnPaint(wxPaintEvent&) {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();

        std::vector<unsigned char> px;
        int w = 0, h = 0;
        wxString overlay;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            px = m_pixels;
            w  = m_img_w;
            h  = m_img_h;
            overlay = m_overlay;
        }

        if (w > 0 && h > 0 && static_cast<int>(px.size()) == w * h * 3) {
            wxImage img(w, h, px.data(), /*static_data=*/true);
            const double scale_x = static_cast<double>(sz.x) / w;
            const double scale_y = static_cast<double>(sz.y) / h;
            const double scale   = std::min(scale_x, scale_y);
            const int dw = static_cast<int>(w * scale);
            const int dh = static_cast<int>(h * scale);
            wxBitmap bmp(img.Scale(dw, dh, wxIMAGE_QUALITY_BILINEAR));
            dc.DrawBitmap(bmp, (sz.x - dw) / 2, (sz.y - dh) / 2, false);

            dc.SetTextForeground(wxColour(220, 30, 30));
            wxFont f2(9, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                      wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
            dc.SetFont(f2);
            dc.DrawText(m_label, 8, 8);
            if (!overlay.IsEmpty()) {
                dc.DrawText(overlay, 8, 26);
            }
        } else {
            dc.SetTextForeground(wxColour(120, 120, 120));
            wxFont f(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                     wxFONTWEIGHT_NORMAL, false, wxT("Segoe UI"));
            dc.SetFont(f);
            dc.DrawText(m_label, 8, 8);
            dc.DrawText(wxT("No data"), sz.x / 2 - 30, sz.y / 2);
        }

        DrawPatientPlaneGlyph(dc, sz, m_plane);
    }

    void OnSize(wxSizeEvent& e) { Refresh(); e.Skip(); }

    void OnMouseWheel(wxMouseEvent& e) {
        if (!m_interactive_enabled) {
            e.Skip();
            return;
        }
        if (m_on_slice_wheel) {
            const int delta = (e.GetWheelRotation() > 0) ? 1 : -1;
            m_on_slice_wheel(delta);
        }
    }

    void OnLeftDown(wxMouseEvent& e) {
        if (!m_interactive_enabled) {
            e.Skip();
            return;
        }
        m_dragging = true;
        m_last_mouse = e.GetPosition();
        CaptureMouse();
    }

    void OnLeftUp(wxMouseEvent& e) {
        if (m_dragging) {
            m_dragging = false;
            if (HasCapture()) ReleaseMouse();
        }
        e.Skip();
    }

    void OnMouseMove(wxMouseEvent& e) {
        if (!m_interactive_enabled || !m_dragging || !e.LeftIsDown()) {
            e.Skip();
            return;
        }
        const wxPoint p = e.GetPosition();
        const int dx = p.x - m_last_mouse.x;
        const int dy = p.y - m_last_mouse.y;
        m_last_mouse = p;
        // Drag events no longer control W/L (using auto window/level)
    }

    wxString m_label;
    PatientPlane m_plane;
    wxString m_overlay;
    std::mutex m_mutex;
    std::vector<unsigned char> m_pixels;
    int  m_img_w = 0;
    int  m_img_h = 0;
    bool m_dirty = false;
    bool m_interactive_enabled = false;
    bool m_dragging = false;
    wxPoint m_last_mouse{0, 0};
    std::function<void(int)> m_on_slice_wheel;

};

// ---------------------------------------------------------------------------
// Volume -> slice helpers
// ---------------------------------------------------------------------------
static std::vector<float> ExtractAxialSliceRaw(
    const fdk::Volume3D& vol, int z) {
    if (!vol.valid() || z < 0 || z >= vol.nz) return {};
    const int W = vol.nx, H = vol.ny;
    std::vector<float> out(static_cast<std::size_t>(W * H));
    std::size_t k = 0;
    for (int iy = 0; iy < H; ++iy)
        for (int ix = 0; ix < W; ++ix)
            out[k++] = vol.at(ix, iy, z);
    return out;
}

static std::vector<float> ExtractCoronalSliceRaw(
    const fdk::Volume3D& vol, int y) {
    if (!vol.valid() || y < 0 || y >= vol.ny) return {};
    const int W = vol.nx, H = vol.nz;
    std::vector<float> out(static_cast<std::size_t>(W * H));
    std::size_t k = 0;
    for (int iz = 0; iz < H; ++iz)
        for (int ix = 0; ix < W; ++ix)
            out[k++] = vol.at(ix, y, iz);
    return out;
}

static std::vector<float> ExtractSagittalSliceRaw(
    const fdk::Volume3D& vol, int x) {
    if (!vol.valid() || x < 0 || x >= vol.nx) return {};
    const int W = vol.ny, H = vol.nz;
    std::vector<float> out(static_cast<std::size_t>(W * H));
    std::size_t k = 0;
    for (int iz = 0; iz < H; ++iz)
        for (int iy = 0; iy < W; ++iy)
            out[k++] = vol.at(x, iy, iz);
    return out;
}

static std::vector<float> TransposeSliceRaw(const std::vector<float>& src,
                                            int width,
                                            int height,
                                            bool flip_vertical = false) {
    if (width <= 0 || height <= 0 || static_cast<int>(src.size()) != width * height) {
        return {};
    }

    std::vector<float> out(static_cast<std::size_t>(width) * height);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const int out_row = flip_vertical ? (width - 1 - col) : col;
            out[static_cast<std::size_t>(out_row) * height + row] =
                src[static_cast<std::size_t>(row) * width + col];
        }
    }
    return out;
}

static std::vector<float> RotateSlice180Raw(const std::vector<float>& src,
                                            int width,
                                            int height) {
    if (width <= 0 || height <= 0 || static_cast<int>(src.size()) != width * height) {
        return {};
    }

    std::vector<float> out(static_cast<std::size_t>(width) * height);
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            out[static_cast<std::size_t>(height - 1 - row) * width + (width - 1 - col)] =
                src[static_cast<std::size_t>(row) * width + col];
        }
    }
    return out;
}

static std::vector<unsigned char> ConvertSliceToRgb(
    const std::vector<float>& src, float ww, float wl) {
    std::vector<unsigned char> rgb(static_cast<std::size_t>(src.size() * 3));
    ww = std::max(1.0f, ww);
    const float lo = wl - 0.5f * ww;
    const float hi = wl + 0.5f * ww;
    const float denom = std::max(1e-6f, hi - lo);

    std::size_t k = 0;
    for (float v : src) {
        const float t = std::clamp((v - lo) / denom, 0.0f, 1.0f);
        const unsigned char g = static_cast<unsigned char>(t * 255.0f);
        rgb[k++] = g;
        rgb[k++] = g;
        rgb[k++] = g;
    }
    return rgb;
}

// ---------------------------------------------------------------------------
// FdkReviewFrame  – online reconstruction viewer
// ---------------------------------------------------------------------------
class FdkReviewFrame : public wxFrame {
public:
    FdkReviewFrame(wxWindow* parent, const fdk::FdkParams& params)
        : wxFrame(parent, wxID_ANY, wxT("3D FDK Reconstruction"),
                  wxDefaultPosition, wxSize(1400, 860))
        , m_params(params) {
        SetMinSize(wxSize(900, 600));

        wxPanel* root = new wxPanel(this);
        root->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));

        wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);

        // ---- 4-panel grid ---------------------------------------------
        wxBoxSizer* grid = new wxBoxSizer(wxHORIZONTAL);
        wxBoxSizer* left_col  = new wxBoxSizer(wxVERTICAL);
        wxBoxSizer* right_col = new wxBoxSizer(wxVERTICAL);

        m_axial_panel    = new SlicePanel(root, wxT("AXIAL"), PatientPlane::Axial);
        m_coronal_panel  = new SlicePanel(root, wxT("CORONAL"), PatientPlane::Coronal);
        m_sagittal_panel = new SlicePanel(root, wxT("SAGITTAL"), PatientPlane::Sagittal);

        m_axial_panel->SetInteractionEnabled(false);
        m_coronal_panel->SetInteractionEnabled(false);
        m_sagittal_panel->SetInteractionEnabled(false);

        m_axial_panel->SetSliceWheelCallback([this](int delta) {
            m_slice_y += delta * std::max(1, m_slice_step);
            RenderCurrentSlices();
        });
        m_coronal_panel->SetSliceWheelCallback([this](int delta) {
            if (LatestVolumeLooksLikeRtkPhysicalHead()) {
                m_slice_x += delta * std::max(1, m_slice_step);
            } else {
                m_slice_y += delta * std::max(1, m_slice_step);
            }
            RenderCurrentSlices();
        });
        m_sagittal_panel->SetSliceWheelCallback([this](int delta) {
            m_slice_z += delta * std::max(1, m_slice_step);
            RenderCurrentSlices();
        });

        // Auto window/level adjustment - no manual W/L drag control

        left_col->Add(m_axial_panel,    1, wxALL | wxEXPAND, 4);
        left_col->Add(m_sagittal_panel, 1, wxALL | wxEXPAND, 4);
        grid->Add(left_col, 1, wxEXPAND);

        // Right column: coronal + session log
        right_col->Add(m_coronal_panel, 1, wxALL | wxEXPAND, 4);

        // Session log panel
        wxPanel* log_panel = new wxPanel(root, wxID_ANY, wxDefaultPosition,
                                         wxDefaultSize, wxBORDER_SIMPLE);
        log_panel->SetBackgroundColour(*wxWHITE);
        wxBoxSizer* log_sz = new wxBoxSizer(wxVERTICAL);

        wxStaticText* log_title = new wxStaticText(log_panel, wxID_ANY, wxT("Session Log"));
        wxFont log_title_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                              wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
        log_title->SetFont(log_title_font);
        log_title->SetForegroundColour(wxColour(50, 50, 52));
        log_sz->Add(log_title, 0, wxALL, 6);

        m_log_ctrl = new wxTextCtrl(log_panel, wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxDefaultSize,
                                    wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
        wxFont log_font(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL,
                        wxFONTWEIGHT_NORMAL, false, wxT("Consolas"));
        m_log_ctrl->SetFont(log_font);
        m_log_ctrl->SetBackgroundColour(*wxWHITE);
        m_log_ctrl->SetForegroundColour(*wxBLACK);
        log_sz->Add(m_log_ctrl, 1, wxALL | wxEXPAND, 4);
        log_panel->SetSizer(log_sz);
        log_panel->SetMinSize(wxSize(-1, 80));

        right_col->Add(log_panel, 1, wxALL | wxEXPAND, 4);
        grid->Add(right_col, 1, wxEXPAND);

        top->Add(grid, 1, wxEXPAND | wxALL, 4);

        // ---- Bottom status/action row ---------------------------------
        wxPanel* footer = new wxPanel(root);
        footer->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
        wxBoxSizer* ft_sz = new wxBoxSizer(wxHORIZONTAL);

        m_status_label = new wxStaticText(footer, wxID_ANY, wxT("Idle"));
        wxFont st_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                   wxFONTWEIGHT_NORMAL, false, wxT("Segoe UI"));
        m_status_label->SetFont(st_font);
        m_status_label->SetForegroundColour(wxColour(0, 140, 0));
        ft_sz->Add(m_status_label, 1, wxALL | wxALIGN_CENTER_VERTICAL, 8);

        wxStaticText* step_lbl = new wxStaticText(footer, wxID_ANY, wxT("Step for slice"));
        ft_sz->Add(step_lbl, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, 4);
        m_slice_step_ctrl = new wxSpinCtrl(footer, wxID_ANY, wxT("1"),
            wxDefaultPosition, wxSize(56, -1), wxSP_ARROW_KEYS, 1, 20, 1);
        m_slice_step_ctrl->Bind(wxEVT_SPINCTRL, &FdkReviewFrame::OnSliceStepChanged, this);
        ft_sz->Add(m_slice_step_ctrl, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 8);



        m_reset_btn = new wxButton(footer, wxID_ANY, wxT("Reset View"));
        m_reset_btn->SetMinSize(wxSize(100, 32));
        m_reset_btn->Enable(false);
        m_reset_btn->Bind(wxEVT_BUTTON, &FdkReviewFrame::OnResetView, this);
        ft_sz->Add(m_reset_btn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);

        m_export_btn = new wxButton(footer, wxID_ANY, wxT("Export DICOM Volume"));
        m_export_btn->SetMinSize(wxSize(170, 32));
        m_export_btn->Enable(false);
        m_export_btn->Bind(wxEVT_BUTTON, &FdkReviewFrame::OnExportDicom, this);
        ft_sz->Add(m_export_btn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);

        m_close_btn = new wxButton(footer, wxID_ANY, wxT("Close"));
        m_close_btn->SetMinSize(wxSize(90, 32));
        m_close_btn->Bind(wxEVT_BUTTON, &FdkReviewFrame::OnCloseBtn, this);
        ft_sz->Add(m_close_btn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);

        footer->SetSizer(ft_sz);
        top->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

        root->SetSizer(top);
        Centre();

        // ---- Bind custom events ----------------------------------------
        Bind(wxEVT_FDK_LOG,      &FdkReviewFrame::OnLogEvent,      this);
        Bind(wxEVT_FDK_PROGRESS, &FdkReviewFrame::OnProgressEvent, this);
        Bind(wxEVT_FDK_COMPLETE, &FdkReviewFrame::OnCompleteEvent, this);
        Bind(wxEVT_FDK_ERROR,    &FdkReviewFrame::OnErrorEvent,    this);
        Bind(wxEVT_CLOSE_WINDOW, &FdkReviewFrame::OnClose,         this);

        // ---- Refresh timer for slice views -----------------------------
        m_refresh_timer.SetOwner(this);
        Bind(wxEVT_TIMER, &FdkReviewFrame::OnRefreshTimer, this);
        m_refresh_timer.Start(500); // refresh display every 500 ms

        AppendLog(wxT("FDK reconstruction initialised."));
        AppendLog(wxString::Format(
            wxT("Volume: %d x %d x %d  voxel=%.3f cm"),
            m_params.nx, m_params.ny, m_params.nz, m_params.voxel_size_cm));
        AppendLog(wxString::Format(
            wxT("FDD=%.1f cm  FID=%.1f cm  filter=%s"),
            m_params.fdd_cm, m_params.fid_cm,
            wxString::FromUTF8(m_params.filter)));
        AppendLog(wxString::Format(
            wxT("Detector: %d x %d  size=%.2f cm"),
            m_params.detector_cols, m_params.detector_rows, m_params.detector_size_cm));
        AppendLog(wxT("INI: ") + wxString::FromUTF8(m_params.ini_config_file));
        AppendLog(wxT("Projection folder: ") + wxString::FromUTF8(m_params.image_dir));
        AppendLog(wxT("Frame info: ") + wxString::FromUTF8(m_params.frame_info_file));
        AppendLog(wxT("Gain file: ") + wxString::FromUTF8(m_params.gain_file));
        AppendLog(wxT("RTK reconstruction publishes the volume after filter update; slice views will render when reconstruction completes."));
        AppendLog(wxT("Starting reconstruction thread..."));

        // ---- Launch reconstruction thread ------------------------------
        m_stop_flag.store(false);
        m_recon_thread = std::thread(&FdkReviewFrame::ReconThreadFunc, this);
    }

    ~FdkReviewFrame() override {
        m_stop_flag.store(true);
        if (m_recon_thread.joinable()) m_recon_thread.join();
    }

private:
    // ----- Reconstruction thread ----------------------------------------
    void ReconThreadFunc() {
        try {
        auto send_log = [this](const std::string& msg) {
            wxCommandEvent ev(wxEVT_FDK_LOG);
            ev.SetString(wxString::FromUTF8(msg));
            AddPendingEvent(ev);
        };

        auto send_progress = [this](const fdk::ReconProgress& rp) {
            wxCommandEvent ev(wxEVT_FDK_PROGRESS);
            ev.SetString(wxString::FromUTF8(rp.message));
            ev.SetInt(static_cast<int>(rp.pct));
            AddPendingEvent(ev);
        };

        auto send_fdk_log = [&](const std::string& msg) {
            if (msg.empty()) {
                return;
            }
            if (msg.rfind("[FDK]", 0) == 0) {
                send_log(msg);
            } else {
                send_log("[FDK] " + msg);
            }
        };

        send_log("[FDK] Loading frame info...");

        // Load frame info (or generate synthetic angles if file not provided)
        std::vector<fdk::FrameInfo> frames;
        if (!m_params.frame_info_file.empty() &&
            std::filesystem::exists(m_params.frame_info_file)) {
            std::string err;
            if (!fdk::LoadFrameInfo(m_params.frame_info_file, frames, err)) {
                wxCommandEvent ev(wxEVT_FDK_ERROR);
                ev.SetString(wxString::FromUTF8(err));
                AddPendingEvent(ev);
                return;
            }
            send_log("[FDK] Loaded " + std::to_string(frames.size()) + " frames from file.");
        } else {
            // Auto-generate uniform angles from start to stop
            const int n_proj = 360;
            const float step = (m_params.gantry_stop_angle_deg - m_params.gantry_start_angle_deg)
                               / static_cast<float>(n_proj);
            for (int i = 0; i < n_proj; ++i) {
                fdk::FrameInfo fi;
                fi.frame_id         = i;
                fi.gantry_angle_deg = m_params.gantry_start_angle_deg + i * step;
                frames.push_back(fi);
            }
            send_log("[FDK] No frame info file - using " + std::to_string(n_proj) +
                     " auto-generated angles.");
        }

        // Load projections
        send_log("[FDK] Loading projection images from: " + m_params.image_dir);
        std::vector<fdk::ProjectionImage> projections;
        std::string load_err;
        const bool loaded = fdk::LoadProjections(
            m_params, frames, projections,
            [&](int done, int total, const std::string& msg) {
                fdk::ReconProgress rp;
                rp.frames_done  = done;
                rp.frames_total = total;
                rp.pct          = 5.0f * done / std::max(1, total);
                rp.message      = msg;
                send_progress(rp);
                send_fdk_log(msg);
            },
            load_err);

        if (!loaded) {
            // Use synthetic phantom projections for demonstration
            send_log("[FDK] " + load_err);
            send_log("[FDK] Generating synthetic phantom projections for demonstration...");
            projections = GenerateSyntheticProjections(frames, m_params);
            send_log("[FDK] Generated " + std::to_string(projections.size()) +
                     " synthetic projections.");
        } else {
            send_log("[FDK] Loaded " + std::to_string(projections.size()) +
                     " projections successfully.");
            
            // Log first frame detector offset info for diagnostics
            if (!projections.empty()) {
                const auto& first_proj = projections[0];
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "[FDK] Frame 0 detector offset: u_centre=%.2f px, v_centre=%.2f px, "
                    "proj size %dx%d, gantry_angle=%.1f°",
                    first_proj.u_centre, first_proj.v_centre,
                    first_proj.width, first_proj.height,
                    first_proj.gantry_angle_deg);
                send_log(buf);
            }
        }

        if (m_stop_flag.load()) return;

        send_log("[FDK] Starting RTK FDK reconstruction...");
        send_log("[FDK] Live three-plane preview is disabled for the RTK backend; final axial/coronal/sagittal views will render after the output volume is complete.");
        fdk::FdkEngine engine(m_params);
        fdk::Volume3D final_vol;
        const bool reconstructed = engine.Reconstruct(
            projections, final_vol,
            [&](const fdk::ReconProgress& rp) {
                send_progress(rp);
                send_fdk_log(rp.message);
            });

        if (!reconstructed) {
            wxCommandEvent ev(wxEVT_FDK_ERROR);
            ev.SetString(wxT("RTK reconstruction failed. See session log for details."));
            AddPendingEvent(ev);
            return;
        }

        if (!m_stop_flag.load()) {

            // Log robust intensity stats to Session Log (GUI app has no console by default)
            if (final_vol.valid() && !final_vol.data.empty()) {
                float vmin = final_vol.data.front();
                float vmax = final_vol.data.front();
                for (float v : final_vol.data) {
                    if (v < vmin) vmin = v;
                    if (v > vmax) vmax = v;
                }

                std::vector<float> sample;
                sample.reserve(std::min<std::size_t>(200000, final_vol.data.size()));
                const std::size_t stride = std::max<std::size_t>(1, final_vol.data.size() / 200000);
                for (std::size_t i = 0; i < final_vol.data.size(); i += stride) {
                    sample.push_back(final_vol.data[i]);
                }
                std::sort(sample.begin(), sample.end());
                const auto pick_pct = [&](double p) -> float {
                    if (sample.empty()) return 0.0f;
                    const std::size_t idx = static_cast<std::size_t>(
                        std::clamp(p, 0.0, 1.0) * static_cast<double>(sample.size() - 1));
                    return sample[idx];
                };

                const float p01 = pick_pct(0.01);
                const float p99 = pick_pct(0.99);
                send_log("[FDK] Final volume stats: min=" + std::to_string(vmin) +
                         " max=" + std::to_string(vmax) +
                         " p01=" + std::to_string(p01) +
                         " p99=" + std::to_string(p99));
            }

            StoreReconstructedVolume(std::move(final_vol));

            wxCommandEvent ev(wxEVT_FDK_COMPLETE);
            ev.SetString(wxT("Reconstruction complete."));
            AddPendingEvent(ev);
        }
        } catch (const std::exception& ex) {
            wxCommandEvent ev(wxEVT_FDK_ERROR);
            ev.SetString(wxT("Reconstruction failed: ") + wxString::FromUTF8(ex.what()));
            AddPendingEvent(ev);
        } catch (...) {
            wxCommandEvent ev(wxEVT_FDK_ERROR);
            ev.SetString(wxT("Reconstruction failed with an unknown native exception."));
            AddPendingEvent(ev);
        }
    }

    // ----- Synthetic phantom generator (for demo when no real data) ------
    static std::vector<fdk::ProjectionImage> GenerateSyntheticProjections(
        const std::vector<fdk::FrameInfo>& frames,
        const fdk::FdkParams& params) {
        // Simple ellipsoid phantom: Shepp-Logan like
        const int W = std::min(params.detector_cols, 256);
        const int H = std::min(params.detector_rows, 256);
        const float W_half = 0.5f * (W - 1);
        const float H_half = 0.5f * (H - 1);
        const float r_u    = 0.35f * W;
        const float r_v    = 0.45f * H;

        std::vector<fdk::ProjectionImage> out;
        out.reserve(frames.size());

        for (const auto& fi : frames) {
            fdk::ProjectionImage proj;
            proj.width            = W;
            proj.height           = H;
            proj.gantry_angle_deg = fi.gantry_angle_deg;
            proj.frame_id         = fi.frame_id;
            proj.pixels.resize(static_cast<std::size_t>(W * H));

            const float angle_rad = fi.gantry_angle_deg * static_cast<float>(M_PI) / 180.0f;

            for (int v = 0; v < H; ++v) {
                for (int u = 0; u < W; ++u) {
                    const float uf = (u - W_half) / r_u;
                    const float vf = (v - H_half) / r_v;
                    // Ellipsoid chord length along ray (simplification)
                    const float cos_a = std::cos(angle_rad);
                    const float sum   = uf * uf + vf * vf;
                    float val = 0.0f;
                    if (sum < 1.0f) {
                        val = 2.0f * std::sqrt(1.0f - sum) * 0.3f;
                    }
                    // Inner ball
                    const float uf2 = (u - W_half * (1 + 0.1f * cos_a)) / (r_u * 0.3f);
                    const float vf2 = (v - H_half * 0.8f) / (r_v * 0.25f);
                    if (uf2 * uf2 + vf2 * vf2 < 1.0f) {
                        val += 0.2f;
                    }
                    proj.pixels[static_cast<std::size_t>(v * W + u)] = val;
                }
            }
            out.push_back(std::move(proj));
        }
        return out;
    }

    // Store reconstruction output from the worker thread. Rendering happens on the UI thread.
    void StoreReconstructedVolume(fdk::Volume3D&& vol) {
        if (!vol.valid()) return;
        {
            std::lock_guard<std::mutex> lock(m_volume_mutex);
            m_latest_volume = std::move(vol);
            if (!m_interaction_enabled) {
                m_slice_x = m_latest_volume.nx / 2;
                m_slice_y = m_latest_volume.ny / 2;
                m_slice_z = m_latest_volume.nz / 2;
                float lo = 0.0f, hi = 1.0f;
                ComputeMinMax(m_latest_volume, lo, hi);
                m_wl = 0.5f * (lo + hi);
                m_ww = std::max(1.0f, hi - lo);
            }
        }
    }

    static void ComputeMinMax(const fdk::Volume3D& vol, float& lo, float& hi) {
        // Use robust percentiles to avoid outliers blowing up WW/WL and making image appear black.
        if (vol.data.empty()) {
            lo = 0.0f;
            hi = 1.0f;
            return;
        }

        std::vector<float> sample;
        sample.reserve(std::min<std::size_t>(200000, vol.data.size()));
        const std::size_t stride = std::max<std::size_t>(1, vol.data.size() / 200000);
        for (std::size_t i = 0; i < vol.data.size(); i += stride) {
            sample.push_back(vol.data[i]);
        }

        if (sample.empty()) {
            lo = 0.0f;
            hi = 1.0f;
            return;
        }

        std::sort(sample.begin(), sample.end());
        const std::size_t n = sample.size();
        const std::size_t i_lo = static_cast<std::size_t>(0.01 * static_cast<double>(n - 1));
        const std::size_t i_hi = static_cast<std::size_t>(0.99 * static_cast<double>(n - 1));
        lo = sample[i_lo];
        hi = sample[i_hi];

        // Fallback: if robust range collapsed, use full min/max range.
        if (hi <= lo) {
            lo = sample.front();
            hi = sample.back();
        }
        if (hi <= lo) {
            hi = lo + 1.0f;
        }
    }

    void RenderCurrentSlices() {
        fdk::Volume3D vol;
        {
            std::lock_guard<std::mutex> lock(m_volume_mutex);
            if (!m_latest_volume.valid()) return;
            vol = m_latest_volume;
        }

        m_slice_z = std::clamp(m_slice_z, 0, std::max(0, vol.nz - 1));
        m_slice_y = std::clamp(m_slice_y, 0, std::max(0, vol.ny - 1));
        m_slice_x = std::clamp(m_slice_x, 0, std::max(0, vol.nx - 1));

        const bool looks_like_rtk_physical_head_volume = (vol.nx == vol.nz && vol.ny < vol.nx);
        std::vector<float> sagittal_raw;
        std::vector<float> coronal_raw;
        std::vector<float> axial_raw;
        int sagittal_w = 0;
        int sagittal_h = 0;
        int coronal_w = 0;
        int coronal_h = 0;
        int axial_w = 0;
        int axial_h = 0;

        if (looks_like_rtk_physical_head_volume) {
            // RTK physical volume: X/Z are transverse axes, Y is rotation/SI axis.
            sagittal_raw = ExtractAxialSliceRaw(vol, m_slice_z);
            coronal_raw  = TransposeSliceRaw(ExtractSagittalSliceRaw(vol, m_slice_x), vol.ny, vol.nz);
            axial_raw    = ExtractCoronalSliceRaw(vol, m_slice_y);
            sagittal_w = vol.nx; sagittal_h = vol.ny;
            coronal_w = vol.nz; coronal_h = vol.ny;
            axial_w = vol.nx; axial_h = vol.nz;
        } else {
            // AVL SCAN storage volume: SAGITTAL <- Z, CORONAL <- Y, AXIAL <- X.
            sagittal_raw = TransposeSliceRaw(ExtractAxialSliceRaw(vol, m_slice_z), vol.nx, vol.ny);
            coronal_raw  = TransposeSliceRaw(ExtractCoronalSliceRaw(vol, m_slice_y), vol.nx, vol.nz);
            axial_raw    = ExtractSagittalSliceRaw(vol, m_slice_x);
            sagittal_w = vol.ny; sagittal_h = vol.nx;
            coronal_w = vol.nz; coronal_h = vol.nx;
            axial_w = vol.ny; axial_h = vol.nz;
            sagittal_raw = RotateSlice180Raw(sagittal_raw, sagittal_w, sagittal_h);
            coronal_raw = RotateSlice180Raw(coronal_raw, coronal_w, coronal_h);
        }

        auto axial_rgb    = ConvertSliceToRgb(axial_raw, m_ww, m_wl);
        auto coronal_rgb  = ConvertSliceToRgb(coronal_raw, m_ww, m_wl);
        auto sagittal_rgb = ConvertSliceToRgb(sagittal_raw, m_ww, m_wl);

        const wxString ww_wl = wxString::Format(wxT("W %.1f  L %.1f"), m_ww, m_wl);
        m_sagittal_panel->SetSlice(
            sagittal_rgb, sagittal_w, sagittal_h,
            wxString::Format(wxT("Slice %d/%d  |  %s"),
                             m_slice_z,
                             std::max(0, vol.nz - 1),
                             ww_wl));
        m_coronal_panel->SetSlice(
            coronal_rgb, coronal_w, coronal_h,
            wxString::Format(wxT("Slice %d/%d  |  %s"),
                             looks_like_rtk_physical_head_volume ? m_slice_x : m_slice_y,
                             looks_like_rtk_physical_head_volume ? std::max(0, vol.nx - 1) : std::max(0, vol.ny - 1),
                             ww_wl));
        m_axial_panel->SetSlice(
            axial_rgb, axial_w, axial_h,
            wxString::Format(wxT("Slice %d/%d  |  %s"),
                             looks_like_rtk_physical_head_volume ? m_slice_y : m_slice_x,
                             looks_like_rtk_physical_head_volume ? std::max(0, vol.ny - 1) : std::max(0, vol.nx - 1),
                             ww_wl));

        m_axial_panel->RefreshSlice();
        m_coronal_panel->RefreshSlice();
        m_sagittal_panel->RefreshSlice();
    }

    bool LatestVolumeLooksLikeRtkPhysicalHead() {
        std::lock_guard<std::mutex> lock(m_volume_mutex);
        return m_latest_volume.valid() &&
               m_latest_volume.nx == m_latest_volume.nz &&
               m_latest_volume.ny < m_latest_volume.nx;
    }

    void ResetViewToDefaults() {
        fdk::Volume3D vol;
        {
            std::lock_guard<std::mutex> lock(m_volume_mutex);
            if (!m_latest_volume.valid()) return;
            vol = m_latest_volume;
        }
        m_slice_x = vol.nx / 2;
        m_slice_y = vol.ny / 2;
        m_slice_z = vol.nz / 2;
        float lo = 0.0f, hi = 1.0f;
        ComputeMinMax(vol, lo, hi);
        m_wl = 0.5f * (lo + hi);
        m_ww = std::max(1.0f, hi - lo);
        RenderCurrentSlices();
    }

    bool ExportVolumeAsDicom(const std::string& out_dir, std::string& err) {
        fdk::Volume3D vol;
        {
            std::lock_guard<std::mutex> lock(m_volume_mutex);
            if (!m_latest_volume.valid()) {
                err = "No reconstructed volume available.";
                return false;
            }
            vol = m_latest_volume;
        }

        return dicom_export::ExportFdkVolume(vol, m_params, out_dir, err);
    }

    // ----- GUI event handlers -------------------------------------------
    void AppendLog(const wxString& msg) {
        const wxString line = wxT("[") + NowStr() + wxT("] ") + msg + wxT("\n");
        m_log_ctrl->AppendText(line);
        m_log_ctrl->ShowPosition(m_log_ctrl->GetLastPosition());
    }

    void OnLogEvent(wxCommandEvent& e) {
        AppendLog(e.GetString());
    }

    void OnProgressEvent(wxCommandEvent& e) {
        const int pct = e.GetInt();
        m_status_label->SetLabel(
            wxString::Format(wxT("%.0f%%  %s"), static_cast<float>(pct), e.GetString()));
    }

    void OnCompleteEvent(wxCommandEvent& e) {
        m_interaction_enabled = true;
        m_axial_panel->SetInteractionEnabled(true);
        m_coronal_panel->SetInteractionEnabled(true);
        m_sagittal_panel->SetInteractionEnabled(true);
        m_export_btn->Enable(true);
        m_reset_btn->Enable(true);

        m_status_label->SetLabel(wxT("Done"));
        AppendLog(e.GetString());
        AppendLog(wxT("Volume reconstruction finished successfully."));
        m_refresh_timer.Stop();
        RenderCurrentSlices();
    }

    void OnErrorEvent(wxCommandEvent& e) {
        m_status_label->SetLabel(wxT("Error"));
        m_status_label->SetForegroundColour(wxColour(220, 80, 80));
        AppendLog(wxT("ERROR: ") + e.GetString());
        m_export_btn->Enable(false);
    }

    void OnResetView(wxCommandEvent&) {
        ResetViewToDefaults();
    }

    void OnSliceStepChanged(wxSpinEvent& e) {
        m_slice_step = std::max(1, e.GetPosition());
    }



    void OnCloseBtn(wxCommandEvent&) {
        m_stop_flag.store(true);
        Close();
    }

    void OnExportDicom(wxCommandEvent&) {
        wxDirDialog dlg(this, wxT("Select export directory"), wxEmptyString,
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) {
            return;
        }
        const std::string out_dir = std::string(dlg.GetPath().mb_str());
        std::string err;
        if (ExportVolumeAsDicom(out_dir, err)) {
            AppendLog(wxT("DICOM export completed: ") + dlg.GetPath());
            wxMessageBox(wxT("DICOM export completed."), wxT("Export"), wxOK | wxICON_INFORMATION, this);
        } else {
            AppendLog(wxT("DICOM export failed: ") + wxString::FromUTF8(err));
            wxMessageBox(wxString::FromUTF8(err), wxT("Export"), wxOK | wxICON_WARNING, this);
        }
    }

    void OnClose(wxCloseEvent& e) {
        m_stop_flag.store(true);
        m_refresh_timer.Stop();
        if (m_recon_thread.joinable()) m_recon_thread.join();
        e.Skip();
    }

    void OnRefreshTimer(wxTimerEvent&) {
        m_axial_panel   ->RefreshSlice();
        m_coronal_panel ->RefreshSlice();
        m_sagittal_panel->RefreshSlice();
    }

    // ----- Members -------------------------------------------------------
    fdk::FdkParams  m_params;

    SlicePanel* m_axial_panel    = nullptr;
    SlicePanel* m_coronal_panel  = nullptr;
    SlicePanel* m_sagittal_panel = nullptr;

    wxTextCtrl*  m_log_ctrl     = nullptr;
    wxStaticText* m_status_label = nullptr;
    wxSpinCtrl*  m_slice_step_ctrl = nullptr;
    wxButton*    m_reset_btn    = nullptr;
    wxButton*    m_close_btn     = nullptr;
    wxButton*    m_export_btn    = nullptr;

    wxTimer      m_refresh_timer;
    std::thread  m_recon_thread;
    std::atomic<bool> m_stop_flag{false};
    std::mutex   m_volume_mutex;
    fdk::Volume3D m_latest_volume;
    int m_slice_x = 0;
    int m_slice_y = 0;
    int m_slice_z = 0;
    int m_slice_step = 1;
    float m_ww = 1.0f;
    float m_wl = 0.0f;
    float m_ww_drag_scale = 2.0f;
    float m_wl_drag_scale = 1.5f;
    bool m_interaction_enabled = false;
};

// ---------------------------------------------------------------------------
// FdkStartupDialog – parameter input window
// Mirrors the XVI startup dialog / registration startup dialog pattern.
// ---------------------------------------------------------------------------
class FdkStartupDialog : public wxDialog {
public:
    explicit FdkStartupDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, wxT("3D FDK Reconstruction - Setup"),
                   wxDefaultPosition, wxSize(660, 760),
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        SetMinSize(wxSize(520, 620));
        BuildUI();
        if (s_has_last_params) {
            PopulateControlsFromParams(s_last_params);
        }
        EnsureExistingInputDefaults(/*force_defaults=*/true);
        ApplyIniToControls(m_ini_ctrl->GetValue(), /*show_message=*/false);
    }

    fdk::FdkParams GetParams() const { return m_params; }

private:
    bool ApplyIniToControls(const wxString& ini_path, bool show_message) {
        if (ini_path.IsEmpty() || !wxFileExists(ini_path)) {
            if (show_message) {
                wxMessageBox(wxT("Please enter a valid INI file path first."),
                             wxT("Load INI"), wxOK | wxICON_INFORMATION, this);
            }
            return false;
        }

        fdk::IniConfig cfg;
        fdk::FdkParams tmp;
        if (!cfg.load(std::string(ini_path.mb_str()))) {
            if (show_message) {
                wxMessageBox(wxT("Failed to parse INI file."), wxT("Load INI"),
                             wxOK | wxICON_WARNING, this);
            }
            return false;
        }

        cfg.apply_to_params(tmp);

        m_nx_ctrl->SetValue(tmp.nx);
        m_ny_ctrl->SetValue(tmp.ny);
        m_nz_ctrl->SetValue(tmp.nz);
        m_vox_ctrl->SetValue(wxString::Format(wxT("%.4f"), tmp.voxel_size_cm));
        m_fdd_ctrl->SetValue(wxString::Format(wxT("%.2f"), tmp.fdd_cm));
        m_fid_ctrl->SetValue(wxString::Format(wxT("%.2f"), tmp.fid_cm));
        m_det_size_ctrl->SetValue(wxString::Format(wxT("%.2f"), tmp.detector_size_cm));
        m_det_cols_ctrl->SetValue(tmp.detector_cols);
        m_det_rows_ctrl->SetValue(tmp.detector_rows);
        m_start_angle_ctrl->SetValue(wxString::Format(wxT("%.1f"), tmp.gantry_start_angle_deg));
        m_stop_angle_ctrl->SetValue(wxString::Format(wxT("%.1f"), tmp.gantry_stop_angle_deg));
        m_filter_a_ctrl->SetValue(wxString::Format(wxT("%.3f"), tmp.filter_param_a));
        m_snr_ctrl->SetValue(wxString::Format(wxT("%.1f"), tmp.filter_snr));
        m_short_scan_chk->SetValue(tmp.short_scan);
        m_scale_ctrl->SetValue(wxString::Format(wxT("%.1f"), tmp.scale_out));
        m_offset_out_ctrl->SetValue(wxString::Format(wxT("%.1f"), tmp.offset_out));

        // Set filter choice
        const wxString filter_str = wxString::FromUTF8(tmp.filter);
        const int fi = m_filter_choice->FindString(filter_str, /*bCase=*/false);
        if (fi != wxNOT_FOUND) m_filter_choice->SetSelection(fi);
        if (!tmp.gain_file.empty()) {
            m_gain_ctrl->SetValue(wxString::FromUTF8(tmp.gain_file));
        }

        if (show_message) {
            wxMessageBox(wxT("INI loaded successfully."), wxT("Load INI"), wxOK, this);
        }
        return true;
    }

    void OnIniPathChanged(wxCommandEvent&) {
        const wxString ini_path = m_ini_ctrl->GetValue();
        if (ini_path.IsEmpty() || !wxFileExists(ini_path)) {
            return;
        }
        ApplyIniToControls(ini_path, /*show_message=*/false);
    }

    // Populate all controls from an FdkParams struct (used for last-session restore)
    void PopulateControlsFromParams(const fdk::FdkParams& p) {
        m_img_dir_ctrl->SetValue(wxString::FromUTF8(p.image_dir));
        m_gain_ctrl->SetValue(wxString::FromUTF8(p.gain_file));
        m_frame_ctrl->SetValue(wxString::FromUTF8(p.frame_info_file));
        m_ini_ctrl->SetValue(wxString::FromUTF8(p.ini_config_file));
        m_nx_ctrl->SetValue(p.nx);
        m_ny_ctrl->SetValue(p.ny);
        m_nz_ctrl->SetValue(p.nz);
        m_vox_ctrl->SetValue(wxString::Format(wxT("%.4f"), p.voxel_size_cm));
        m_fdd_ctrl->SetValue(wxString::Format(wxT("%.2f"), p.fdd_cm));
        m_fid_ctrl->SetValue(wxString::Format(wxT("%.2f"), p.fid_cm));
        m_det_size_ctrl->SetValue(wxString::Format(wxT("%.2f"), p.detector_size_cm));
        m_det_cols_ctrl->SetValue(p.detector_cols);
        m_det_rows_ctrl->SetValue(p.detector_rows);
        m_start_angle_ctrl->SetValue(wxString::Format(wxT("%.1f"), p.gantry_start_angle_deg));
        m_stop_angle_ctrl->SetValue(wxString::Format(wxT("%.1f"), p.gantry_stop_angle_deg));
        const int fi = m_filter_choice->FindString(wxString::FromUTF8(p.filter), false);
        if (fi != wxNOT_FOUND) m_filter_choice->SetSelection(fi);
        m_filter_a_ctrl->SetValue(wxString::Format(wxT("%.3f"), p.filter_param_a));
        m_snr_ctrl->SetValue(wxString::Format(wxT("%.1f"), p.filter_snr));
        m_short_scan_chk->SetValue(p.short_scan);
        m_scale_ctrl->SetValue(wxString::Format(wxT("%.1f"), p.scale_out));
        m_offset_out_ctrl->SetValue(wxString::Format(wxT("%.1f"), p.offset_out));
        m_threads_ctrl->SetValue(p.num_threads);
    }

    void BuildUI() {
        wxBoxSizer* root_sz = new wxBoxSizer(wxVERTICAL);

        // ----- Title --------------------------------------------------------
        wxStaticText* title = new wxStaticText(this, wxID_ANY,
            wxT("3D FDK Reconstruction Parameters"));
        wxFont tf(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                  wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
        title->SetFont(tf);
        root_sz->Add(title, 0, wxALL, 12);
        root_sz->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

        wxScrolledWindow* scroll = new wxScrolledWindow(this, wxID_ANY,
            wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        scroll->SetScrollRate(0, 10);
        wxBoxSizer* sc_sz = new wxBoxSizer(wxVERTICAL);

        auto AddSection = [&](const wxString& label) {
            wxStaticText* lbl = new wxStaticText(scroll, wxID_ANY, label);
            wxFont sf(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL,
                      wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
            lbl->SetFont(sf);
            lbl->SetForegroundColour(wxColour(50, 100, 200));
            sc_sz->Add(lbl, 0, wxLEFT | wxTOP, 10);
            sc_sz->Add(new wxStaticLine(scroll), 0, wxEXPAND | wxALL, 4);
        };

        auto MakeRow = [&](const wxString& label, wxWindow* ctrl) {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            wxStaticText* lbl_w = new wxStaticText(scroll, wxID_ANY, label);
            lbl_w->SetMinSize(wxSize(200, -1));
            row->Add(lbl_w, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
            row->Add(ctrl,  1, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, 6);
            sc_sz->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
        };

        auto MakeBrowseRow = [&](const wxString& label,
                                  wxTextCtrl*& text_out,
                                  const wxString& btn_label,
                                  bool is_dir,
                                  const wxString& filter = wxT("All files (*.*)|*.*")) {
            wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
            wxStaticText* lbl_w = new wxStaticText(scroll, wxID_ANY, label);
            lbl_w->SetMinSize(wxSize(200, -1));
            row->Add(lbl_w, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
            text_out = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString);
            row->Add(text_out, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
            wxButton* browse = new wxButton(scroll, wxID_ANY, btn_label,
                wxDefaultPosition, wxSize(80, -1));
            if (is_dir) {
                browse->Bind(wxEVT_BUTTON, [this, text_out](wxCommandEvent&) {
                    const wxString initial_dir = ExistingDirectoryOrFdkDataRoot(text_out->GetValue());
                    wxDirDialog dlg(this, wxT("Select folder"), initial_dir,
                                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
                    if (dlg.ShowModal() == wxID_OK) text_out->SetValue(dlg.GetPath());
                });
            } else {
                wxString flt = filter;
                browse->Bind(wxEVT_BUTTON, [this, text_out, flt](wxCommandEvent&) {
                    const wxString current_path = text_out->GetValue();
                    wxString initial_dir = ExistingDirectoryOrFdkDataRoot(current_path);
                    wxString initial_file;
                    if (!current_path.IsEmpty()) {
                        wxFileName file_name(current_path);
                        if (file_name.FileExists()) {
                            initial_file = file_name.GetFullName();
                        }
                    }
                    wxFileDialog dlg(this, wxT("Select file"), initial_dir, initial_file,
                                     flt, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
                    if (dlg.ShowModal() == wxID_OK) text_out->SetValue(dlg.GetPath());
                });
            }
            row->Add(browse, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxLEFT, 4);
            sc_sz->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
        };

        // ---- Section: Input files ----------------------------------------
        AddSection(wxT("Input Files"));

        MakeBrowseRow(wxT("2D Projection Image Dir:"), m_img_dir_ctrl,
                      wxT("Browse"), /*is_dir=*/true);
        MakeBrowseRow(wxT("Gain File (.his):"), m_gain_ctrl,
                      wxT("Browse"), false,
                      wxT("HIS files (*.his)|*.his|All files (*.*)|*.*"));
        MakeBrowseRow(wxT("Frame Info File:"), m_frame_ctrl,
                      wxT("Browse"), false,
                      wxT("Frame info (*.txt;*.dat;*.FrameIDs;*.xml)|*.txt;*.dat;*.FrameIDs;*.xml|All files (*.*)|*.*"));
        MakeBrowseRow(wxT("INI Config File:"), m_ini_ctrl,
                      wxT("Browse"), false,
                      wxT("INI files (*.ini)|*.ini|All files (*.*)|*.*"));
        m_img_dir_ctrl->SetValue(DefaultFdkProjectionDir());
        m_gain_ctrl->SetValue(DefaultFdkGainFile());
        m_frame_ctrl->SetValue(DefaultFdkFrameInfoFile());
        m_ini_ctrl->SetValue(DefaultFdkIniFile());
        m_ini_ctrl->Bind(wxEVT_TEXT, &FdkStartupDialog::OnIniPathChanged, this);

        // ---- Section: Volume geometry -----------------------------------
        AddSection(wxT("Volume Geometry"));

        m_nx_ctrl = new wxSpinCtrl(scroll, wxID_ANY, wxT("256"),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 32, 1024, 256);
        MakeRow(wxT("Volume Dim X (voxels):"), m_nx_ctrl);

        m_ny_ctrl = new wxSpinCtrl(scroll, wxID_ANY, wxT("256"),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 32, 1024, 256);
        MakeRow(wxT("Volume Dim Y (voxels):"), m_ny_ctrl);

        m_nz_ctrl = new wxSpinCtrl(scroll, wxID_ANY, wxT("256"),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 32, 1024, 256);
        MakeRow(wxT("Volume Dim Z (voxels):"), m_nz_ctrl);

        m_vox_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("0.100"));
        MakeRow(wxT("Voxel Size (cm):"), m_vox_ctrl);

        // ---- Section: Scan geometry -------------------------------------
        AddSection(wxT("Scan Geometry"));

        m_fdd_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("153.6"));
        MakeRow(wxT("FDD - Focus to Detector (cm):"), m_fdd_ctrl);

        m_fid_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("100.0"));
        MakeRow(wxT("FID - Focus to Isocenter (cm):"), m_fid_ctrl);

        m_det_size_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("40.0"));
        MakeRow(wxT("Detector Size (cm):"), m_det_size_ctrl);

        m_det_cols_ctrl = new wxSpinCtrl(scroll, wxID_ANY, wxT("512"),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 64, 2048, 512);
        MakeRow(wxT("Detector Columns (pixels):"), m_det_cols_ctrl);

        m_det_rows_ctrl = new wxSpinCtrl(scroll, wxID_ANY, wxT("512"),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 64, 2048, 512);
        MakeRow(wxT("Detector Rows (pixels):"), m_det_rows_ctrl);

        m_start_angle_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("0.0"));
        MakeRow(wxT("Gantry Start Angle (deg):"), m_start_angle_ctrl);

        m_stop_angle_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("360.0"));
        MakeRow(wxT("Gantry Stop Angle (deg):"), m_stop_angle_ctrl);

        // ---- Section: Filter & Reconstruction ---------------------------
        AddSection(wxT("Filter & Reconstruction"));

        m_filter_choice = new wxChoice(scroll, wxID_ANY);
        m_filter_choice->Append(wxT("Wiener"));
        m_filter_choice->Append(wxT("Hamming"));
        m_filter_choice->Append(wxT("RamLak"));
        m_filter_choice->SetSelection(0);
        MakeRow(wxT("Ramp Filter:"), m_filter_choice);

        m_filter_a_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("0.5"));
        MakeRow(wxT("Filter Param A:"), m_filter_a_ctrl);

        m_snr_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("60.0"));
        MakeRow(wxT("Filter SNR (Wiener):"), m_snr_ctrl);

        m_short_scan_chk = new wxCheckBox(scroll, wxID_ANY, wxT("Short scan (Parker weighting)"));
        sc_sz->Add(m_short_scan_chk, 0, wxLEFT | wxBOTTOM, 14);

        m_scale_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("1000.0"));
        MakeRow(wxT("Scale Out:"), m_scale_ctrl);

        m_offset_out_ctrl = new wxTextCtrl(scroll, wxID_ANY, wxT("-1000.0"));
        MakeRow(wxT("Offset Out (HU):"), m_offset_out_ctrl);

        m_threads_ctrl = new wxSpinCtrl(scroll, wxID_ANY, wxT("4"),
            wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 32, 4);
        MakeRow(wxT("Back-projection Threads:"), m_threads_ctrl);

        scroll->SetSizer(sc_sz);
        root_sz->Add(scroll, 1, wxEXPAND | wxALL, 4);

        root_sz->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 10);

        // ---- OK / Cancel -----------------------------------------------
        wxBoxSizer* btn_row = new wxBoxSizer(wxHORIZONTAL);
        btn_row->AddStretchSpacer();
        wxButton* ok_btn = new wxButton(this, wxID_OK, wxT("Start Reconstruction"));
        ok_btn->SetBackgroundColour(wxColour(48, 176, 199));
        ok_btn->SetForegroundColour(*wxWHITE);
        ok_btn->SetMinSize(wxSize(160, 36));
        btn_row->Add(ok_btn, 0, wxALL, 8);

        wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, wxT("Cancel"));
        cancel_btn->SetMinSize(wxSize(80, 36));
        btn_row->Add(cancel_btn, 0, wxALL, 8);

        ok_btn->Bind(wxEVT_BUTTON, &FdkStartupDialog::OnOK, this);
        root_sz->Add(btn_row, 0, wxEXPAND);

        SetSizer(root_sz);
    }

    void EnsureExistingInputDefaults(bool force_defaults = false) {
        if (m_img_dir_ctrl && (force_defaults || !wxDirExists(m_img_dir_ctrl->GetValue()))) {
            m_img_dir_ctrl->SetValue(DefaultFdkProjectionDir());
        }
        if (m_gain_ctrl && (force_defaults || !wxFileExists(m_gain_ctrl->GetValue()))) {
            m_gain_ctrl->SetValue(DefaultFdkGainFile());
        }
        if (m_frame_ctrl && (force_defaults || !wxFileExists(m_frame_ctrl->GetValue()))) {
            m_frame_ctrl->SetValue(DefaultFdkFrameInfoFile());
        }
        if (m_ini_ctrl && (force_defaults || !wxFileExists(m_ini_ctrl->GetValue()))) {
            m_ini_ctrl->SetValue(DefaultFdkIniFile());
        }
    }

    bool ValidateInputsBeforeRun() {
        const wxString image_dir = m_img_dir_ctrl ? m_img_dir_ctrl->GetValue() : wxString();
        if (image_dir.IsEmpty() || !wxDirExists(image_dir)) {
            wxMessageBox(wxT("Please select an existing 2D projection image directory."),
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }

        const wxString gain_file = m_gain_ctrl ? m_gain_ctrl->GetValue() : wxString();
        if (!gain_file.IsEmpty() && !wxFileExists(gain_file)) {
            wxMessageBox(wxT("Gain file does not exist:\n") + gain_file,
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }

        const wxString frame_file = m_frame_ctrl ? m_frame_ctrl->GetValue() : wxString();
        if (!frame_file.IsEmpty() && !wxFileExists(frame_file)) {
            wxMessageBox(wxT("Frame info file does not exist:\n") + frame_file,
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }

        const wxString ini_file = m_ini_ctrl ? m_ini_ctrl->GetValue() : wxString();
        if (!ini_file.IsEmpty() && !wxFileExists(ini_file)) {
            wxMessageBox(wxT("INI config file does not exist:\n") + ini_file,
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }

        double value = 0.0;
        if (!m_vox_ctrl->GetValue().ToDouble(&value) || value <= 0.0) {
            wxMessageBox(wxT("Voxel Size must be a positive number."),
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }
        if (!m_fdd_ctrl->GetValue().ToDouble(&value) || value <= 0.0) {
            wxMessageBox(wxT("FDD must be a positive number."),
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }
        if (!m_fid_ctrl->GetValue().ToDouble(&value) || value <= 0.0) {
            wxMessageBox(wxT("FID must be a positive number."),
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }
        if (!m_det_size_ctrl->GetValue().ToDouble(&value) || value <= 0.0) {
            wxMessageBox(wxT("Detector Size must be a positive number."),
                         wxT("FDK Reconstruction"), wxOK | wxICON_WARNING, this);
            return false;
        }
        return true;
    }

    void OnOK(wxCommandEvent&) {
        if (!ValidateInputsBeforeRun()) {
            return;
        }

        // Collect fields into m_params
        m_params.image_dir       = std::string(m_img_dir_ctrl->GetValue().mb_str());
        m_params.gain_file       = std::string(m_gain_ctrl->GetValue().mb_str());
        m_params.frame_info_file = std::string(m_frame_ctrl->GetValue().mb_str());
        m_params.ini_config_file = std::string(m_ini_ctrl->GetValue().mb_str());

        m_params.nx = m_nx_ctrl->GetValue();
        m_params.ny = m_ny_ctrl->GetValue();
        m_params.nz = m_nz_ctrl->GetValue();

        double d = 0.0;
        if (m_vox_ctrl->GetValue().ToDouble(&d) && d > 0)
            m_params.voxel_size_cm = static_cast<float>(d);

        if (m_fdd_ctrl->GetValue().ToDouble(&d) && d > 0)
            m_params.fdd_cm = static_cast<float>(d);
        if (m_fid_ctrl->GetValue().ToDouble(&d) && d > 0)
            m_params.fid_cm = static_cast<float>(d);
        if (m_det_size_ctrl->GetValue().ToDouble(&d) && d > 0)
            m_params.detector_size_cm = static_cast<float>(d);

        m_params.detector_cols = m_det_cols_ctrl->GetValue();
        m_params.detector_rows = m_det_rows_ctrl->GetValue();

        if (m_start_angle_ctrl->GetValue().ToDouble(&d))
            m_params.gantry_start_angle_deg = static_cast<float>(d);
        if (m_stop_angle_ctrl->GetValue().ToDouble(&d))
            m_params.gantry_stop_angle_deg = static_cast<float>(d);

        m_params.filter = std::string(
            m_filter_choice->GetString(m_filter_choice->GetSelection()).mb_str());

        if (m_filter_a_ctrl->GetValue().ToDouble(&d))
            m_params.filter_param_a = static_cast<float>(d);
        if (m_snr_ctrl->GetValue().ToDouble(&d) && d > 0)
            m_params.filter_snr = static_cast<float>(d);

        m_params.short_scan = m_short_scan_chk->GetValue();

        if (m_scale_ctrl->GetValue().ToDouble(&d))
            m_params.scale_out = static_cast<float>(d);
        if (m_offset_out_ctrl->GetValue().ToDouble(&d))
            m_params.offset_out = static_cast<float>(d);

        m_params.num_threads = m_threads_ctrl->GetValue();

        // Load INI overrides if provided
        if (!m_params.ini_config_file.empty() &&
            std::filesystem::exists(m_params.ini_config_file)) {
            fdk::IniConfig cfg;
            if (cfg.load(m_params.ini_config_file)) {
                cfg.apply_to_params(m_params);
            }
        }

        EndModal(wxID_OK);
    }

    fdk::FdkParams m_params;

    // File pickers
    wxTextCtrl* m_img_dir_ctrl  = nullptr;
    wxTextCtrl* m_gain_ctrl     = nullptr;
    wxTextCtrl* m_frame_ctrl    = nullptr;
    wxTextCtrl* m_ini_ctrl      = nullptr;

    // Volume geometry
    wxSpinCtrl* m_nx_ctrl  = nullptr;
    wxSpinCtrl* m_ny_ctrl  = nullptr;
    wxSpinCtrl* m_nz_ctrl  = nullptr;
    wxTextCtrl* m_vox_ctrl = nullptr;

    // Scan geometry
    wxTextCtrl* m_fdd_ctrl         = nullptr;
    wxTextCtrl* m_fid_ctrl         = nullptr;
    wxTextCtrl* m_det_size_ctrl    = nullptr;
    wxSpinCtrl* m_det_cols_ctrl    = nullptr;
    wxSpinCtrl* m_det_rows_ctrl    = nullptr;
    wxTextCtrl* m_start_angle_ctrl = nullptr;
    wxTextCtrl* m_stop_angle_ctrl  = nullptr;

    // Filter
    wxChoice*   m_filter_choice    = nullptr;
    wxTextCtrl* m_filter_a_ctrl    = nullptr;
    wxTextCtrl* m_snr_ctrl         = nullptr;
    wxCheckBox* m_short_scan_chk   = nullptr;
    wxTextCtrl* m_scale_ctrl       = nullptr;
    wxTextCtrl* m_offset_out_ctrl  = nullptr;
    wxSpinCtrl* m_threads_ctrl     = nullptr;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace fdk_ui {

void ShowReconstructionDialog(wxWindow* parent) {
    FdkStartupDialog dlg(parent);
    if (dlg.ShowModal() != wxID_OK) return;

    const fdk::FdkParams params = dlg.GetParams();
    s_last_params     = params;
    s_has_last_params = true;

    FdkReviewFrame* frame = new FdkReviewFrame(nullptr, params);
    frame->Show();
}

} // namespace fdk_ui
