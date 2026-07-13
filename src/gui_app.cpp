#include "gui_app.h"

#include <wx/dirdlg.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>

#include "dicom_viewer.h"

MainFrame::MainFrame(const wxString& title)
    : wxFrame(nullptr, wxID_ANY, title, wxDefaultPosition, wxSize(1620, 1080)) {
    SetMinSize(wxSize(1380, 920));

    wxPanel* panel = new wxPanel(this);
    panel->SetBackgroundColour(wxColour(242, 242, 247));

    wxBoxSizer* frame_sizer = new wxBoxSizer(wxVERTICAL);

    wxPanel* header_panel = new wxPanel(panel);
    header_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* header_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* brand_title = new wxStaticText(header_panel, wxID_ANY, wxT("Imaging Studio"));
    wxFont brand_font(17, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
    brand_title->SetFont(brand_font);
    brand_title->SetForegroundColour(wxColour(28, 28, 30));
    header_sizer->Add(brand_title, 0, wxLEFT | wxRIGHT | wxTOP, 16);

    wxStaticText* brand_subtitle = new wxStaticText(
        header_panel, wxID_ANY, wxT("Acquisition flow and RAW preview"));
    wxFont sub_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("Segoe UI"));
    brand_subtitle->SetFont(sub_font);
    brand_subtitle->SetForegroundColour(wxColour(99, 99, 102));
    header_sizer->Add(brand_subtitle, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);
    header_panel->SetSizer(header_sizer);

    wxBoxSizer* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxPanel* left_card = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    left_card->SetBackgroundColour(*wxWHITE);
    left_card->SetMinSize(wxSize(360, 860));
    wxBoxSizer* left_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* workflow_title = new wxStaticText(left_card, wxID_ANY, wxT("Workflow"));
    wxFont workflow_font(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
    workflow_title->SetFont(workflow_font);
    workflow_title->SetForegroundColour(wxColour(28, 28, 30));
    left_sizer->Add(workflow_title, 0, wxALL, 14);

    auto style_button = [](wxButton* button, const wxColour& bg, const wxColour& fg) {
        button->SetBackgroundColour(bg);
        button->SetForegroundColour(fg);
        button->SetMinSize(wxSize(280, 46));
        wxFont btn_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxT("Segoe UI"));
        button->SetFont(btn_font);
    };

    wxButton* start_btn = new wxButton(left_card, wxID_ANY, wxT("Start Acquisition"));
    start_btn->Bind(wxEVT_BUTTON, &MainFrame::OnStartAcquisition, this);
    style_button(start_btn, wxColour(0, 122, 255), *wxWHITE);
    left_sizer->Add(start_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 14);

    wxButton* end_btn = new wxButton(left_card, wxID_ANY, wxT("End Acquisition"));
    end_btn->Bind(wxEVT_BUTTON, &MainFrame::OnEndAcquisition, this);
    style_button(end_btn, wxColour(255, 59, 48), *wxWHITE);
    left_sizer->Add(end_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 14);

    wxButton* dicom_btn = new wxButton(left_card, wxID_ANY, wxT("Load 3D DICOM Folder"));
    dicom_btn->Bind(wxEVT_BUTTON, &MainFrame::OnLoadDicomFolder, this);
    style_button(dicom_btn, wxColour(52, 199, 89), *wxWHITE);
    left_sizer->Add(dicom_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 14);

    wxButton* info_btn = new wxButton(left_card, wxID_ANY, wxT("Show Platform Info"));
    info_btn->Bind(wxEVT_BUTTON, &MainFrame::OnShowPlatformInfo, this);
    style_button(info_btn, wxColour(229, 229, 234), wxColour(28, 28, 30));
    left_sizer->Add(info_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 14);

    wxStaticText* log_label = new wxStaticText(left_card, wxID_ANY, wxT("Session Log"));
    wxFont log_label_font(10, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
    log_label->SetFont(log_label_font);
    log_label->SetForegroundColour(wxColour(99, 99, 102));
    left_sizer->Add(log_label, 0, wxLEFT | wxRIGHT | wxTOP, 14);

    m_text_ctrl = new wxTextCtrl(
        left_card, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(320, 420),
        wxTE_MULTILINE | wxTE_READONLY);
    m_text_ctrl->SetBackgroundColour(wxColour(250, 250, 252));
    m_text_ctrl->SetForegroundColour(wxColour(28, 28, 30));
    left_sizer->Add(m_text_ctrl, 1, wxALL | wxEXPAND, 14);

    wxButton* exit_btn = new wxButton(left_card, wxID_EXIT, wxT("Exit"));
    exit_btn->Bind(wxEVT_BUTTON, &MainFrame::OnQuit, this);
    style_button(exit_btn, wxColour(229, 229, 234), wxColour(99, 99, 102));
    left_sizer->Add(exit_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 14);
    left_card->SetSizer(left_sizer);

    wxPanel* right_card = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_SIMPLE);
    right_card->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* right_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* raw_title = new wxStaticText(right_card, wxID_ANY, wxT("RAW Imaging Canvas"));
    wxFont raw_title_font(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD, false, wxT("Segoe UI"));
    raw_title->SetFont(raw_title_font);
    raw_title->SetForegroundColour(wxColour(28, 28, 30));
    right_sizer->Add(raw_title, 0, wxALL, 14);

    wxStaticText* raw_hint = new wxStaticText(right_card, wxID_ANY, wxT("Display target: 1024 x 1024 RAW grayscale frame"));
    raw_hint->SetForegroundColour(wxColour(99, 99, 102));
    right_sizer->Add(raw_hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, 14);

    m_raw_view_panel = new wxPanel(right_card, wxID_ANY, wxDefaultPosition, wxSize(1024, 1024), wxBORDER_SIMPLE);
    m_raw_view_panel->SetBackgroundColour(wxColour(18, 18, 18));
    m_raw_view_panel->SetMinSize(wxSize(1024, 1024));
    right_sizer->Add(m_raw_view_panel, 0, wxALL | wxCENTER, 14);
    right_card->SetSizer(right_sizer);

    content_sizer->Add(left_card, 0, wxALL | wxEXPAND, 12);
    content_sizer->Add(right_card, 1, wxALL | wxEXPAND, 12);

    frame_sizer->Add(header_panel, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, 10);
    frame_sizer->Add(content_sizer, 1, wxALL | wxEXPAND, 8);

    panel->SetSizer(frame_sizer);
    Centre();

    // Display initial greeting
    wxString welcome_msg = wxString::FromUTF8(
        hello_cross_platform::build_message());
    m_text_ctrl->AppendText(welcome_msg + wxT("\n\n"));
    m_text_ctrl->AppendText(wxT("Workflow:\n"));
    m_text_ctrl->AppendText(wxT("1) Click 'Start Acquisition' to begin workflow\n"));
    m_text_ctrl->AppendText(wxT("2) RAW viewer area is shown on the right (1024x1024)\n"));
    m_text_ctrl->AppendText(wxT("3) Click 'End Acquisition' to stop\n\n"));
}

void MainFrame::OnStartAcquisition(wxCommandEvent& event) {
    (void)event;
    m_text_ctrl->AppendText(wxT("Start acquisition clicked.\n"));
}

void MainFrame::OnEndAcquisition(wxCommandEvent& event) {
    (void)event;
    m_text_ctrl->AppendText(wxT("End acquisition clicked.\n"));
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

    wxDirDialog dialog(
        this,
        wxT("Select DICOM directory"),
        wxEmptyString,
        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);

    if (dialog.ShowModal() != wxID_OK) {
        return;
    }

    const wxString folder = dialog.GetPath();
    m_text_ctrl->AppendText(wxT("Loading DICOM folder: ") + folder + wxT("\n"));

    std::string error_message;
    const bool ok = dicom_viewer::ShowDicomVolume(
        std::string(folder.mb_str()), error_message);

    if (!ok) {
        const wxString wx_error = wxString::FromUTF8(error_message);
        m_text_ctrl->AppendText(wxT("Failed to load DICOM volume: ") + wx_error + wxT("\n"));
        wxMessageBox(
            wxT("Failed to load DICOM volume:\n") + wx_error,
            wxT("DICOM Error"),
            wxOK | wxICON_ERROR,
            this);
        return;
    }

    m_text_ctrl->AppendText(wxT("DICOM volume viewer opened.\n"));
}

void MainFrame::OnQuit(wxCommandEvent& event) {
    (void)event;
    Close(true);
}

bool HelloApp::OnInit() {
    MainFrame* frame = new MainFrame(wxT("Hello Cross-Platform"));
    frame->Show();
    return true;
}

wxIMPLEMENT_APP_NO_MAIN(HelloApp);
