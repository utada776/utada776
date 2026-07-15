#pragma once

#include <wx/wx.h>

// Factory function that creates the 3D FDK reconstruction startup dialog.
// Shows a parameter input dialog; on OK opens the online reconstruction window.
namespace fdk_ui {
    void ShowReconstructionDialog(wxWindow* parent);
}
