#pragma once

#include <wx/wx.h>

namespace bean_style {

wxString GameTitle();
wxSize WindowSize();
wxSize MinWindowSize();
wxSize GamePanelSize();
int TimerIntervalMs();

}  // namespace bean_style

class BeanFrame : public wxFrame {
public:
    explicit BeanFrame(wxWindow* parent);
};
