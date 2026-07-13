#pragma once

#include <wx/wx.h>

#include "hello_cross_platform/platform.h"

class MainFrame : public wxFrame {
public:
    MainFrame(const wxString& title);

private:
    void OnStartAcquisition(wxCommandEvent& event);
    void OnEndAcquisition(wxCommandEvent& event);
    void OnShowPlatformInfo(wxCommandEvent& event);
    void OnLoadDicomFolder(wxCommandEvent& event);
    void OnQuit(wxCommandEvent& event);

    wxTextCtrl* m_text_ctrl;
    wxPanel* m_raw_view_panel;
};

class HelloApp : public wxApp {
public:
    bool OnInit() override;
};
