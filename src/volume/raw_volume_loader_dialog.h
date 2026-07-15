#pragma once

#include <wx/wx.h>

#include <string>

#include "dicom/raw_loader.h"

class RawVolumeLoaderDialog : public wxDialog {
public:
    explicit RawVolumeLoaderDialog(wxWindow* parent, const std::string& last_ini_path = "");

    raw_volume::RawVolumeMetadata GetMetadata() const {
        return m_metadata;
    }

private:
    void OnBrowseFile(wxCommandEvent&);
    void OnValidate(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    void LoadDefaultsFromINI(const std::string& ini_path);
    bool ValidateInputs(std::string& error_msg);

    wxTextCtrl* m_file_path_ctrl = nullptr;
    wxSpinCtrl* m_dim_x_ctrl = nullptr;
    wxSpinCtrl* m_dim_y_ctrl = nullptr;
    wxSpinCtrl* m_dim_z_ctrl = nullptr;
    wxTextCtrl* m_voxel_size_ctrl = nullptr;
    wxStaticText* m_validation_label = nullptr;

    raw_volume::RawVolumeMetadata m_metadata;
};
