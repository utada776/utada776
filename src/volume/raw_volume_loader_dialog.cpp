#include "volume/raw_volume_loader_dialog.h"

#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>

#include <fstream>

RawVolumeLoaderDialog::RawVolumeLoaderDialog(wxWindow* parent, const std::string& last_ini_path)
    : wxDialog(parent, wxID_ANY, wxT("Load Raw Volume Data"),
               wxDefaultPosition, wxSize(520, 380),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

    SetMinSize(wxSize(420, 320));

    wxBoxSizer* root_sz = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* file_sz = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* file_lbl = new wxStaticText(this, wxID_ANY, wxT("Raw File:"));
    file_sz->Add(file_lbl, 0, wxALL | wxALIGN_CENTER_VERTICAL, 6);

    m_file_path_ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxDefaultSize,
                                      wxTE_READONLY);
    file_sz->Add(m_file_path_ctrl, 1, wxALL | wxEXPAND, 4);

    wxButton* browse_btn = new wxButton(this, wxID_ANY, wxT("Browse..."));
    browse_btn->Bind(wxEVT_BUTTON, &RawVolumeLoaderDialog::OnBrowseFile, this);
    file_sz->Add(browse_btn, 0, wxALL, 4);

    root_sz->Add(file_sz, 0, wxEXPAND | wxALL, 8);

    root_sz->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    wxStaticText* dims_title = new wxStaticText(this, wxID_ANY, wxT("Dimensions (voxels):"));
    wxFont title_font = dims_title->GetFont();
    title_font.MakeBold();
    dims_title->SetFont(title_font);
    root_sz->Add(dims_title, 0, wxALL, 8);

    wxBoxSizer* dims_sz = new wxBoxSizer(wxHORIZONTAL);

    dims_sz->Add(new wxStaticText(this, wxID_ANY, wxT("X:")), 0,
                 wxALL | wxALIGN_CENTER_VERTICAL, 4);
    m_dim_x_ctrl = new wxSpinCtrl(this, wxID_ANY, wxT("321"),
                                  wxDefaultPosition, wxSize(80, -1),
                                  wxSP_ARROW_KEYS, 1, 4096, 321);
    dims_sz->Add(m_dim_x_ctrl, 0, wxALL, 4);

    dims_sz->Add(new wxStaticText(this, wxID_ANY, wxT("Y:")), 0,
                 wxALL | wxALIGN_CENTER_VERTICAL, 4);
    m_dim_y_ctrl = new wxSpinCtrl(this, wxID_ANY, wxT("410"),
                                  wxDefaultPosition, wxSize(80, -1),
                                  wxSP_ARROW_KEYS, 1, 4096, 410);
    dims_sz->Add(m_dim_y_ctrl, 0, wxALL, 4);

    dims_sz->Add(new wxStaticText(this, wxID_ANY, wxT("Z:")), 0,
                 wxALL | wxALIGN_CENTER_VERTICAL, 4);
    m_dim_z_ctrl = new wxSpinCtrl(this, wxID_ANY, wxT("410"),
                                  wxDefaultPosition, wxSize(80, -1),
                                  wxSP_ARROW_KEYS, 1, 4096, 410);
    dims_sz->Add(m_dim_z_ctrl, 0, wxALL, 4);

    dims_sz->AddStretchSpacer();
    root_sz->Add(dims_sz, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    wxBoxSizer* voxel_sz = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* voxel_lbl = new wxStaticText(this, wxID_ANY, wxT("Voxel Size (cm):"));
    voxel_sz->Add(voxel_lbl, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);

    m_voxel_size_ctrl = new wxTextCtrl(this, wxID_ANY, wxT("0.1"),
                                       wxDefaultPosition, wxSize(100, -1));
    voxel_sz->Add(m_voxel_size_ctrl, 0, wxALL, 4);
    voxel_sz->AddStretchSpacer();
    root_sz->Add(voxel_sz, 0, wxEXPAND | wxLEFT | wxRIGHT, 8);

    root_sz->Add(new wxStaticLine(this), 0, wxEXPAND | wxTOP | wxBOTTOM | wxLEFT | wxRIGHT, 8);

    m_validation_label = new wxStaticText(this, wxID_ANY, wxT("Ready"));
    m_validation_label->SetForegroundColour(wxColour(0, 128, 0));
    root_sz->Add(m_validation_label, 0, wxALL, 8);

    wxBoxSizer* btn_sz = new wxBoxSizer(wxHORIZONTAL);

    wxButton* validate_btn = new wxButton(this, wxID_ANY, wxT("Validate"));
    validate_btn->Bind(wxEVT_BUTTON, &RawVolumeLoaderDialog::OnValidate, this);
    btn_sz->Add(validate_btn, 0, wxALL, 6);

    btn_sz->AddStretchSpacer();

    wxButton* ok_btn = new wxButton(this, wxID_OK, wxT("Load"));
    ok_btn->Bind(wxEVT_BUTTON, &RawVolumeLoaderDialog::OnOK, this);
    btn_sz->Add(ok_btn, 0, wxALL, 6);

    wxButton* cancel_btn = new wxButton(this, wxID_CANCEL, wxT("Cancel"));
    btn_sz->Add(cancel_btn, 0, wxALL, 6);

    root_sz->Add(btn_sz, 0, wxEXPAND | wxALL, 4);

    SetSizer(root_sz);

    if (!last_ini_path.empty()) {
        LoadDefaultsFromINI(last_ini_path);
    }
}

void RawVolumeLoaderDialog::OnBrowseFile(wxCommandEvent&) {
    wxFileDialog dlg(this,
                    wxT("Select Raw Volume File"),
                    wxEmptyString,
                    wxEmptyString,
                    wxT("Raw files (*.raw)|*.raw|All files (*.*)|*.*"),
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dlg.ShowModal() == wxID_OK) {
        m_file_path_ctrl->SetValue(dlg.GetPath());
        m_validation_label->SetLabel(wxT("File selected, click Validate"));
        m_validation_label->SetForegroundColour(wxColour(128, 128, 0));
    }
}

void RawVolumeLoaderDialog::OnValidate(wxCommandEvent&) {
    std::string error;
    if (ValidateInputs(error)) {
        m_validation_label->SetLabel(wxT("File size matches dimensions"));
        m_validation_label->SetForegroundColour(wxColour(0, 128, 0));
    } else {
        m_validation_label->SetLabel(wxString::FromUTF8(error));
        m_validation_label->SetForegroundColour(*wxRED);
    }
    Layout();
}

void RawVolumeLoaderDialog::OnOK(wxCommandEvent& event) {
    std::string error;
    if (!ValidateInputs(error)) {
        wxMessageBox(wxString::FromUTF8(error), wxT("Validation Failed"),
                    wxOK | wxICON_WARNING, this);
        return;
    }

    event.Skip();
}

void RawVolumeLoaderDialog::LoadDefaultsFromINI(const std::string& ini_path) {
    std::ifstream file(ini_path);
    if (!file.is_open()) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        const size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos || eq_pos == 0) {
            continue;
        }

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t\r\n"));
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
        };
        trim(key);
        trim(value);

        if (key == "ReconstructionDimensionX") {
            try {
                m_dim_x_ctrl->SetValue(std::stoi(value));
            } catch (...) {
            }
        } else if (key == "ReconstructionDimensionY") {
            try {
                m_dim_y_ctrl->SetValue(std::stoi(value));
            } catch (...) {
            }
        } else if (key == "ReconstructionDimensionZ") {
            try {
                m_dim_z_ctrl->SetValue(std::stoi(value));
            } catch (...) {
            }
        } else if (key == "ReconstructionVoxelSize") {
            try {
                const float voxel_size = std::stof(value);
                m_voxel_size_ctrl->SetValue(wxString::Format(wxT("%.4f"), voxel_size));
            } catch (...) {
            }
        }
    }
}

bool RawVolumeLoaderDialog::ValidateInputs(std::string& error_msg) {
    const std::string file_path = std::string(m_file_path_ctrl->GetValue().mb_str());
    if (file_path.empty()) {
        error_msg = "Please select a raw file";
        return false;
    }

    const int dim_x = m_dim_x_ctrl->GetValue();
    const int dim_y = m_dim_y_ctrl->GetValue();
    const int dim_z = m_dim_z_ctrl->GetValue();

    if (dim_x <= 0 || dim_y <= 0 || dim_z <= 0) {
        error_msg = "All dimensions must be positive";
        return false;
    }

    float voxel_size = 0.1f;
    try {
        voxel_size = std::stof(std::string(m_voxel_size_ctrl->GetValue().mb_str()));
        if (voxel_size <= 0.0f) {
            error_msg = "Voxel size must be positive";
            return false;
        }
    } catch (...) {
        error_msg = "Invalid voxel size value";
        return false;
    }

    if (!raw_volume::ValidateRawFile(file_path, dim_x, dim_y, dim_z, error_msg)) {
        return false;
    }

    m_metadata.file_path = file_path;
    m_metadata.dim_x = dim_x;
    m_metadata.dim_y = dim_y;
    m_metadata.dim_z = dim_z;
    m_metadata.voxel_size_cm = voxel_size;

    return true;
}
