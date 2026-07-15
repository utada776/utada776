#pragma once

#include <wx/wx.h>

#include "dicom/scan_loader.h"

class ScanVolumeLoaderDialog : public wxDialog {
public:
    explicit ScanVolumeLoaderDialog(wxWindow* parent);

    // Returns metadata after user clicks OK
    scan_volume::ScanVolumeMetadata GetMetadata() const {
        return m_metadata;
    }

private:
    void OnBrowseFile(wxCommandEvent&);
    void OnOK(wxCommandEvent&);

    bool ValidateInputs(std::string& error_msg);

    // UI Controls
    wxTextCtrl* m_file_path_ctrl = nullptr;
    wxStaticText* m_validation_label = nullptr;

    scan_volume::ScanVolumeMetadata m_metadata;
};
