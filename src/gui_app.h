#pragma once

#include <wx/wx.h>

#include "registration/registration_ui.h"
#include "FDK recon/fdk_ui.h"
#include "hello_cross_platform/platform.h"

namespace gui_style {

wxString AppBrandTitle();
wxSize MainWindowSize();
wxSize RawViewSize();

}  // namespace gui_style

class MainFrame : public wxFrame {
public:
    MainFrame(const wxString& title);
    ~MainFrame() override;

private:
    void On3DReconstruction(wxCommandEvent& event);
    void OnLoadRawVolume(wxCommandEvent& event);
    void OnLoadScanVolume(wxCommandEvent& event);
    void OnShowPlatformInfo(wxCommandEvent& event);
    void OnLoadDicomFolder(wxCommandEvent& event);
    void OnOpenPpsDemo(wxCommandEvent& event);
    void OnOpenBeanGame(wxCommandEvent& event);
    void On3DRegistration(wxCommandEvent& event);
    void OnQuit(wxCommandEvent& event);
    void OnHeaderMouseDown(wxMouseEvent& event);
    void OnHeaderMouseMotion(wxMouseEvent& event);
    void OnHeaderMouseUp(wxMouseEvent& event);

    wxTextCtrl* m_text_ctrl = nullptr;
    wxPanel* m_raw_view_panel = nullptr;
    
    // Window drag support
    bool m_is_dragging = false;
    wxPoint m_drag_start_pos;
};

class HelloApp : public wxApp {
public:
    bool OnInit() override;
};

