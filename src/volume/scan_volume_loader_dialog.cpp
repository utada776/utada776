#include "volume/scan_volume_loader_dialog.h"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/statline.h>

#include <filesystem>

ScanVolumeLoaderDialog::ScanVolumeLoaderDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, wxT("Load Scan Volume"),
               wxDefaultPosition, wxSize(560, 200),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

    SetMinSize(wxSize(420, 160));

    wxBoxSizer* root_sz = new wxBoxSizer(wxVERTICAL);

    // ---- File selection row ----
    wxBoxSizer* file_sz = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* file_lbl = new wxStaticText(this, wxID_ANY, wxT("SCAN File:"));
    file_sz->Add(file_lbl, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);

    m_file_path_ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxDefaultSize,
                                      wxTE_READONLY);
    file_sz->Add(m_file_path_ctrl, 1, wxALL | wxEXPAND, 4);

    wxButton* browse_btn = new wxButton(this, wxID_ANY, wxT("Browse..."));
    browse_btn->Bind(wxEVT_BUTTON, &ScanVolumeLoaderDialog::OnBrowseFile, this);
    file_sz->Add(browse_btn, 0, wxALL, 4);

    root_sz->Add(file_sz, 0, wxEXPAND | wxALL, 8);

    root_sz->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    // ---- Status label ----
    m_validation_label = new wxStaticText(this, wxID_ANY, wxT("Select a .SCAN file to load"));
    m_validation_label->SetForegroundColour(wxColour(99, 99, 102));
    root_sz->Add(m_validation_label, 0, wxALL, 8);

    // ---- Buttons ----
    wxBoxSizer* btn_sz = new wxBoxSizer(wxHORIZONTAL);
    btn_sz->AddStretchSpacer();

    wxButton* ok_btn = new wxButton(this, wxID_OK, wxT("Load"));
    ok_btn->Bind(wxEVT_BUTTON, &ScanVolumeLoaderDialog::OnOK, this);
    btn_sz->Add(ok_btn, 0, wxALL, 6);

    wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, wxT("Cancel"));
    btn_sz->Add(cancel_btn, 0, wxALL, 6);

    root_sz->Add(btn_sz, 0, wxEXPAND | wxALL, 4);

    SetSizer(root_sz);
}

void ScanVolumeLoaderDialog::OnBrowseFile(wxCommandEvent&) {
    wxFileDialog dlg(this,
                    wxT("Select SCAN Volume File"),
                    wxEmptyString,
                    wxEmptyString,
                    wxT("SCAN files (*.SCAN)|*.SCAN|All files (*.*)|*.*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() == wxID_OK) {
        m_file_path_ctrl->SetValue(dlg.GetPath());
        m_validation_label->SetLabel(wxT("File selected"));
        m_validation_label->SetForegroundColour(wxColour(0, 128, 0));
        Layout();
    }
}

void ScanVolumeLoaderDialog::OnOK(wxCommandEvent& event) {
    std::string error;
    if (!ValidateInputs(error)) {
        wxMessageBox(wxString::FromUTF8(error), wxT("Validation Failed"),
                    wxOK | wxICON_WARNING, this);
        return;
    }
    event.Skip();
}

bool ScanVolumeLoaderDialog::ValidateInputs(std::string& error_msg) {
    namespace fs = std::filesystem;

    const std::string file_path = std::string(m_file_path_ctrl->GetValue().mb_str());
    if (file_path.empty()) {
        error_msg = "Please select a .SCAN file";
        return false;
    }

    std::error_code ec;
    if (!fs::exists(file_path, ec)) {
        error_msg = "File not found: " + file_path;
        return false;
    }

    m_metadata.file_path = file_path;
    return true;
}
