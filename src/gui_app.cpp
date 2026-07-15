#include "gui_app.h"

#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stdpaths.h>

#include <exception>
#include <filesystem>

#include "bean/bean.h"
#include "dicom/dicom_viewer.h"
#include "dicom/raw_loader.h"
#include "dicom/scan_loader.h"
#include "volume/raw_volume_loader_dialog.h"
#include "volume/scan_volume_loader_dialog.h"
#include "pps/pps.h"

namespace gui_style {

wxString AppBrandTitle() {
    return wxT("Imaging Studio");
}

wxSize MainWindowSize() {
    return wxSize(800, 1080);
}

wxSize RawViewSize() {
    return wxSize(1024, 1024);
}

}  // namespace gui_style

MainFrame::MainFrame(const wxString& title)
    : wxFrame(nullptr, wxID_ANY, title, wxDefaultPosition, gui_style::MainWindowSize()) {
    SetMinSize(wxSize(1380, 920));

    wxPanel* panel = new wxPanel(this);
    panel->SetBackgroundColour(wxColour(242, 242, 247));

    wxBoxSizer* frame_sizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* header_panel = new wxPanel(panel);
    header_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* header_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* brand_title = new wxStaticText(header_panel, wxID_ANY, gui_style::AppBrandTitle());
    brand_title->Bind(wxEVT_LEFT_DOWN, &MainFrame::OnHeaderMouseDown, this);
    brand_title->Bind(wxEVT_MOTION, &MainFrame::OnHeaderMouseMotion, this);
    brand_title->Bind(wxEVT_LEFT_UP, &MainFrame::OnHeaderMouseUp, this);
    wxFont brand_font(17, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
    brand_title->SetFont(brand_font);
    brand_title->SetForegroundColour(wxColour(28, 28, 30));
    header_sizer->Add(brand_title, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    wxStaticText* brand_subtitle = new wxStaticText(
        header_panel, wxID_ANY, wxT("Acquisition flow and RAW preview"));
    brand_subtitle->Bind(wxEVT_LEFT_DOWN, &MainFrame::OnHeaderMouseDown, this);
    brand_subtitle->Bind(wxEVT_MOTION, &MainFrame::OnHeaderMouseMotion, this);
    brand_subtitle->Bind(wxEVT_LEFT_UP, &MainFrame::OnHeaderMouseUp, this);
    wxFont sub_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("Segoe UI"));
    brand_subtitle->SetFont(sub_font);
    brand_subtitle->SetForegroundColour(wxColour(99, 99, 102));
    header_sizer->Add(brand_subtitle, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    header_panel->SetSizer(header_sizer);

    wxBoxSizer* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxPanel* left_card = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    left_card->SetBackgroundColour(*wxWHITE);
    left_card->SetMinSize(wxSize(1220, 860));
    wxBoxSizer* left_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* workflow_title = new wxStaticText(left_card, wxID_ANY, wxT("Workflow"));
    wxFont workflow_font(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
    workflow_title->SetFont(workflow_font);
    workflow_title->SetForegroundColour(wxColour(28, 28, 30));
    left_sizer->Add(workflow_title, 0, wxALL, 14);

    auto style_button = [](wxButton* button, const wxColour& bg, const wxColour& fg) {
        button->SetBackgroundColour(bg);
        button->SetForegroundColour(fg);
        button->SetMinSize(wxSize(0, 46));
        wxFont btn_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("Segoe UI"));
        button->SetFont(btn_font);
    };

    wxBoxSizer* workflow_row1 = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* workflow_row2 = new wxBoxSizer(wxHORIZONTAL);

    wxButton* dicom_btn = new wxButton(left_card, wxID_ANY, wxT("Load 3D DICOM Folder"));
    dicom_btn->Bind(wxEVT_BUTTON, &MainFrame::OnLoadDicomFolder, this);
    style_button(dicom_btn, wxColour(52, 199, 89), *wxWHITE);
    workflow_row1->Add(dicom_btn, 1, wxALL | wxEXPAND, 6);

    wxButton* reconstruction_btn = new wxButton(left_card, wxID_ANY, wxT("3D Reconstruction"));
    reconstruction_btn->Bind(wxEVT_BUTTON, &MainFrame::On3DReconstruction, this);
    style_button(reconstruction_btn, wxColour(48, 176, 199), *wxWHITE);
    workflow_row1->Add(reconstruction_btn, 1, wxALL | wxEXPAND, 6);

    wxButton* load_raw_btn = new wxButton(left_card, wxID_ANY, wxT("Load Raw Volume"));
    load_raw_btn->Bind(wxEVT_BUTTON, &MainFrame::OnLoadRawVolume, this);
    style_button(load_raw_btn, wxColour(122, 161, 178), *wxWHITE);
    workflow_row1->Add(load_raw_btn, 1, wxALL | wxEXPAND, 6);

    wxButton* load_scan_btn = new wxButton(left_card, wxID_ANY, wxT("Load Scan Volume"));
    load_scan_btn->Bind(wxEVT_BUTTON, &MainFrame::OnLoadScanVolume, this);
    style_button(load_scan_btn, wxColour(88, 86, 214), *wxWHITE);
    workflow_row1->Add(load_scan_btn, 1, wxALL | wxEXPAND, 6);

    wxButton* pps_btn = new wxButton(left_card, wxID_ANY, wxT("Open PPS Demo"));
    pps_btn->Bind(wxEVT_BUTTON, &MainFrame::OnOpenPpsDemo, this);
    style_button(pps_btn, wxColour(90, 200, 250), wxColour(20, 20, 20));
    workflow_row2->Add(pps_btn, 1, wxALL | wxEXPAND, 6);

    wxButton* bean_btn = new wxButton(left_card, wxID_ANY, wxT("Open Bean Game"));
    bean_btn->Bind(wxEVT_BUTTON, &MainFrame::OnOpenBeanGame, this);
    style_button(bean_btn, wxColour(255, 214, 10), wxColour(20, 20, 20));
    workflow_row2->Add(bean_btn, 1, wxALL | wxEXPAND, 6);

    wxButton* registration_btn = new wxButton(left_card, wxID_ANY, wxT("3D Registration"));
    registration_btn->Bind(wxEVT_BUTTON, &MainFrame::On3DRegistration, this);
    style_button(registration_btn, wxColour(94, 92, 230), *wxWHITE);
    workflow_row2->Add(registration_btn, 1, wxALL | wxEXPAND, 6);

    wxButton* info_btn = new wxButton(left_card, wxID_ANY, wxT("Show Platform Info"));
    info_btn->Bind(wxEVT_BUTTON, &MainFrame::OnShowPlatformInfo, this);
    style_button(info_btn, wxColour(229, 229, 234), wxColour(28, 28, 30));
    workflow_row2->Add(info_btn, 1, wxALL | wxEXPAND, 6);

    left_sizer->Add(workflow_row1, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);
    left_sizer->Add(workflow_row2, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 8);

    left_sizer->AddStretchSpacer(1);

    wxStaticText* log_label = new wxStaticText(left_card, wxID_ANY, wxT("Session Log"));
    wxFont log_label_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
    log_label->SetFont(log_label_font);
    log_label->SetForegroundColour(wxColour(99, 99, 102));
    left_sizer->Add(log_label, 0, wxLEFT | wxRIGHT | wxTOP, 14);

    m_text_ctrl = new wxTextCtrl(
        left_card, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(320, 180),
        wxTE_MULTILINE | wxTE_READONLY);
    m_text_ctrl->SetBackgroundColour(wxColour(250, 250, 252));
    m_text_ctrl->SetForegroundColour(wxColour(28, 28, 30));
    left_sizer->Add(m_text_ctrl, 0, wxALL | wxEXPAND, 14);

    wxButton* exit_btn = new wxButton(left_card, wxID_EXIT, wxT("Exit"));
    exit_btn->Bind(wxEVT_BUTTON, &MainFrame::OnQuit, this);
    style_button(exit_btn, wxColour(229, 229, 234), wxColour(99, 99, 102));
    left_sizer->Add(exit_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 14);
    left_card->SetSizer(left_sizer);

    content_sizer->Add(left_card, 1, wxALL | wxEXPAND, 12);

    frame_sizer->Add(header_panel, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);
    frame_sizer->Add(content_sizer, 1, wxALL | wxEXPAND, 8);

    panel->SetSizer(frame_sizer);
    Centre();

    // Display initial greeting
    wxString welcome_msg = wxString::FromUTF8(
        hello_cross_platform::build_message());
    m_text_ctrl->AppendText(welcome_msg + wxT("\n\n"));
    m_text_ctrl->AppendText(wxT("Workflow:\n"));
    m_text_ctrl->AppendText(wxT("1) Click 'Load 3D DICOM Folder' to select source data\n"));
    m_text_ctrl->AppendText(wxT("2) Click '3D Reconstruction' to run reconstruction workflow\n"));
    m_text_ctrl->AppendText(wxT("3) Click 'Load Raw Volume' to load raw binary volume data\n"));
    m_text_ctrl->AppendText(wxT("4) Click 'Load Scan Volume' to load a .SCAN volume file\n"));
    m_text_ctrl->AppendText(wxT("5) Click '3D Registration' to open the registration window\n"));
    m_text_ctrl->AppendText(wxT("6) Optional: Open PPS Demo / Bean Game\n\n"));
}

MainFrame::~MainFrame() = default;

void MainFrame::On3DReconstruction(wxCommandEvent& event) {
    (void)event;
    m_text_ctrl->AppendText(wxT("Opening 3D Reconstruction setup...\n"));
    fdk_ui::ShowReconstructionDialog(this);
}

void MainFrame::OnLoadRawVolume(wxCommandEvent& event) {
    (void)event;

    m_text_ctrl->AppendText(wxT("Opening Raw Volume Loader...\n"));

    RawVolumeLoaderDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK) {
        m_text_ctrl->AppendText(wxT("Raw Volume Load cancelled.\n"));
        return;
    }

    auto metadata = dlg.GetMetadata();
    std::string err;

    m_text_ctrl->AppendText(wxT("Loading raw volume: ") + wxString::FromUTF8(metadata.file_path) + wxT("\n"));
    m_text_ctrl->AppendText(wxString::Format(wxT("Dimensions: %d x %d x %d, voxel=%.3f cm\n"),
                                             metadata.dim_x, metadata.dim_y, metadata.dim_z,
                                             metadata.voxel_size_cm));

    auto image = raw_volume::LoadRawVolume(metadata, err);
    if (image == nullptr) {
        m_text_ctrl->AppendText(wxT("ERROR: Load failed - ") + wxString::FromUTF8(err) + wxT("\n"));
        wxMessageBox(wxString::FromUTF8(err), wxT("Load Failed"),
                    wxOK | wxICON_WARNING, this);
        return;
    }

    m_text_ctrl->AppendText(wxT("Raw volume loaded successfully. Opening viewer...\n"));

    if (!dicom_viewer::ShowRawVolume(image, "Raw Volume Viewer", err)) {
        m_text_ctrl->AppendText(wxT("ERROR: Viewer failed - ") + wxString::FromUTF8(err) + wxT("\n"));
        wxMessageBox(wxString::FromUTF8(err), wxT("Viewer Error"),
                    wxOK | wxICON_WARNING, this);
    } else {
        m_text_ctrl->AppendText(wxT("Raw volume viewer closed.\n"));
    }
}

void MainFrame::OnLoadScanVolume(wxCommandEvent& event) {
    (void)event;

    m_text_ctrl->AppendText(wxT("Opening Scan Volume Loader...\n"));

    ScanVolumeLoaderDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK) {
        m_text_ctrl->AppendText(wxT("Scan Volume Load cancelled.\n"));
        return;
    }

    auto metadata = dlg.GetMetadata();
    std::string err;

    m_text_ctrl->AppendText(wxT("Loading SCAN volume: ") + wxString::FromUTF8(metadata.file_path) + wxT("\n"));

    auto image = scan_volume::LoadScanVolume(metadata, err);
    if (image == nullptr) {
        m_text_ctrl->AppendText(wxT("ERROR: Load failed - ") + wxString::FromUTF8(err) + wxT("\n"));
        wxMessageBox(wxString::FromUTF8(err), wxT("Load Failed"),
                    wxOK | wxICON_WARNING, this);
        return;
    }

    m_text_ctrl->AppendText(wxT("SCAN volume loaded successfully. Opening viewer...\n"));

    // Show volume in VTK viewer using dicom_viewer infrastructure
    if (!dicom_viewer::ShowScanVolume(image, "Scan Volume Viewer", err)) {
        m_text_ctrl->AppendText(wxT("ERROR: Viewer failed - ") + wxString::FromUTF8(err) + wxT("\n"));
        wxMessageBox(wxString::FromUTF8(err), wxT("Viewer Error"),
                    wxOK | wxICON_WARNING, this);
    } else {
        m_text_ctrl->AppendText(wxT("Scan volume viewer closed.\n"));
    }
}

void MainFrame::OnShowPlatformInfo(wxCommandEvent& event) {
    (void)event;
    wxString platform = wxString::FromUTF8(
        hello_cross_platform::detect_platform());
    wxString message = wxT("Detected Platform: ") + platform;
    m_text_ctrl->AppendText(message + wxT("\n"));
}

void MainFrame::OnLoadDicomFolder(wxCommandEvent& event) {
    (void)event;

    wxString default_dicom_dir;
    {
        namespace fs = std::filesystem;
        wxString exe_path = wxStandardPaths::Get().GetExecutablePath();
        wxFileName exe_file(exe_path);
        wxString exe_dir = exe_file.GetPath();

        const fs::path cwd_path(wxGetCwd().ToStdWstring());
        const fs::path exe_dir_path(exe_dir.ToStdWstring());
        wxArrayString candidates;
        candidates.Add(wxT("C:\\code test\\avi data\\Bladder Pre Iris"));
        candidates.Add(wxString((cwd_path / L"avi data" / L"Bladder Pre Iris").lexically_normal().wstring()));
        candidates.Add(wxString((exe_dir_path / L".." / L".." / L"avi data" / L"Bladder Pre Iris").lexically_normal().wstring()));

        for (const auto& candidate : candidates) {
            wxFileName candidate_file(candidate);
            candidate_file.Normalize(wxPATH_NORM_ALL);
            const wxString normalized = candidate_file.GetFullPath();
            if (wxDirExists(normalized)) {
                default_dicom_dir = normalized;
                break;
            }
        }
    }

    wxDirDialog dialog(
        this,
        wxT("Select DICOM directory"),
        default_dicom_dir,
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);

    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    wxString folder = dialog.GetPath();
    folder.Trim(true);
    folder.Trim(false);

    if (folder.IsEmpty()) {
        m_text_ctrl->AppendText(wxT("Failed to load DICOM volume: selected folder path is empty.\n"));
        wxMessageBox(
            wxT("Selected folder path is empty."),
            wxT("DICOM Error"),
            wxOK | wxICON_ERROR,
            this);
        return;
    }

    // Ensure path is absolute - use wxGetCwd if relative
    if (!wxIsAbsolutePath(folder)) {
        folder = wxGetCwd() + wxFILE_SEP_PATH + folder;
    }

    if (!wxDirExists(folder)) {
        m_text_ctrl->AppendText(wxT("Failed to load DICOM volume: folder does not exist.\n"));
        wxMessageBox(
            wxT("Selected folder does not exist:\n") + folder,
            wxT("DICOM Error"),
            wxOK | wxICON_ERROR,
            this);
        return;
    }

    m_text_ctrl->AppendText(wxT("Loading DICOM folder: ") + folder + wxT("\n"));

    std::string error_message;
    bool ok = false;
    try {
        ok = dicom_viewer::ShowDicomVolume(folder.ToStdString(), error_message);
    } catch (const std::exception& ex) {
        ok = false;
        error_message = std::string("Unexpected exception while opening DICOM folder: ") + ex.what();
    } catch (...) {
        ok = false;
        error_message = "Unexpected unknown exception while opening DICOM folder.";
    }

    if (!ok) {
        wxString wx_error = wxString::FromUTF8(error_message.c_str());
        if (wx_error.IsEmpty()) {
            wx_error = wxString(error_message.c_str(), wxConvLocal);
        }
        
        // Provide detailed guidance for common errors
        wxString detailed_error = wx_error;
        if (wx_error.Find(wxT("vtkDICOMImageReader")) != wxNOT_FOUND || 
            wx_error.Find(wxT("volDummy")) != wxNOT_FOUND ||
            wx_error.Find(wxT("pathDummy")) != wxNOT_FOUND) {
            detailed_error = wx_error + wxT("\n\n")
                + wxT("This typically means:\n")
                + wxT("1. The folder does not contain valid DICOM files\n")
                + wxT("2. The files are not recognized as DICOM format\n")
                + wxT("3. The folder path contains special characters\n\n")
                + wxT("Please ensure:\n")
                + wxT("- The folder contains actual DICOM medical image files\n")
                + wxT("- Files have .dcm or .dicom extension, or are extension-less DICOM files\n")
                + wxT("- The path does not contain non-ASCII characters");
        }
        
        m_text_ctrl->AppendText(wxT("Failed to load DICOM volume: ") + wx_error + wxT("\n"));
        wxMessageBox(
            wxT("Failed to load DICOM volume:\n") + detailed_error,
            wxT("DICOM Error"),
            wxOK | wxICON_ERROR,
            this);
        return;
    }

    m_text_ctrl->AppendText(wxT("DICOM volume viewer opened.\n"));
}

void MainFrame::OnOpenPpsDemo(wxCommandEvent& event) {
    (void)event;
    PpsFrame* frame = new PpsFrame(this);
    frame->Show();
    m_text_ctrl->AppendText(wxT("PPS demo window opened.\n"));
}

void MainFrame::OnOpenBeanGame(wxCommandEvent& event) {
    (void)event;
    BeanFrame* frame = new BeanFrame(this);
    frame->Show();
    m_text_ctrl->AppendText(wxT("Bean game window opened.\n"));
}

void MainFrame::On3DRegistration(wxCommandEvent& event) {
    (void)event;
    wxFrame* registration_frame = registration_ui::CreateRegistrationWindow(this);
    registration_frame->Show(true);
    m_text_ctrl->AppendText(wxT("3D registration window opened.\n"));
}

void MainFrame::OnQuit(wxCommandEvent& event) {
    (void)event;
    Close(true);
}

void MainFrame::OnHeaderMouseDown(wxMouseEvent& event) {
    (void)event;
    // Capture mouse screen position when header drag starts.
    wxPoint screen_pos = wxGetMousePosition();
    wxPoint frame_pos = GetPosition();
    // Compute drag offset relative to the frame origin.
    m_drag_start_pos = screen_pos - frame_pos;
    m_is_dragging = true;
}

void MainFrame::OnHeaderMouseMotion(wxMouseEvent& event) {
    (void)event;
    if (!m_is_dragging) {
        return;
    }

    // Read current mouse position in screen coordinates.
    wxPoint current_screen_pos = wxGetMousePosition();
    // Convert to new frame top-left using captured drag offset.
    wxPoint new_frame_pos = current_screen_pos - m_drag_start_pos;
    // Move frame.
    SetPosition(new_frame_pos);
}

void MainFrame::OnHeaderMouseUp(wxMouseEvent& event) {
    (void)event;
    m_is_dragging = false;
}

bool HelloApp::OnInit() {
    MainFrame* frame = new MainFrame(wxT("Hello Cross-Platform"));
    frame->Maximize(true);
    frame->Show(true);
    return true;
}

wxIMPLEMENT_APP_NO_MAIN(HelloApp);

