#pragma once

#include <wx/wx.h>

// Factory function that creates the in-process 3D registration window.
// Returned frame uses parent when provided; caller is responsible for Show().
namespace registration_ui {
    wxFrame* CreateRegistrationWindow(wxWindow* parent);
}

