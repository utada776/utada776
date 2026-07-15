#include "pps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbox.h>

class PpsPreviewPanel : public wxPanel {
public:
    PpsPreviewPanel(wxWindow* parent, PpsFrame* owner)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(760, 760), wxBORDER_SIMPLE), owner_(owner) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
    }

private:
    void OnPaint(wxPaintEvent& event) {
        (void)event;
        wxAutoBufferedPaintDC dc(this);
        owner_->RenderPreview(dc, GetClientSize());
    }

    void OnEraseBackground(wxEraseEvent& event) {
        (void)event;
    }

    void OnLeftDown(wxMouseEvent& event) {
        dragging_ = true;
        drag_start_ = event.GetPosition();
        drag_start_elev_ = owner_->elev_;
        drag_start_azim_ = owner_->azim_;
        CaptureMouse();
    }

    void OnLeftUp(wxMouseEvent& event) {
        (void)event;
        if (dragging_) {
            dragging_ = false;
            if (HasCapture()) {
                ReleaseMouse();
            }
        }
    }

    void OnMotion(wxMouseEvent& event) {
        if (!dragging_ || !event.Dragging() || !event.LeftIsDown()) {
            return;
        }

        const wxPoint now = event.GetPosition();
        const int dx = now.x - drag_start_.x;
        const int dy = now.y - drag_start_.y;

        owner_->azim_ = drag_start_azim_ + dx * 0.45;
        owner_->elev_ = std::clamp(drag_start_elev_ - dy * 0.35, -88.0, 88.0);
        owner_->preview_panel_->Refresh(false);
    }

    void OnMouseWheel(wxMouseEvent& event) {
        const int rot = event.GetWheelRotation();
        if (rot == 0) {
            return;
        }
        if (rot > 0) {
            owner_->zoom_scale_ *= 1.08;
        } else {
            owner_->zoom_scale_ *= 0.92;
        }
        owner_->zoom_scale_ = std::clamp(owner_->zoom_scale_, 0.35, 3.0);
        owner_->preview_panel_->Refresh(false);
    }

    PpsFrame* owner_;
    bool dragging_ = false;
    wxPoint drag_start_ = wxPoint(0, 0);
    double drag_start_elev_ = 0.0;
    double drag_start_azim_ = 0.0;

    wxDECLARE_EVENT_TABLE();
};

wxBEGIN_EVENT_TABLE(PpsPreviewPanel, wxPanel)
    EVT_PAINT(PpsPreviewPanel::OnPaint)
    EVT_ERASE_BACKGROUND(PpsPreviewPanel::OnEraseBackground)
    EVT_LEFT_DOWN(PpsPreviewPanel::OnLeftDown)
    EVT_LEFT_UP(PpsPreviewPanel::OnLeftUp)
    EVT_MOTION(PpsPreviewPanel::OnMotion)
    EVT_MOUSEWHEEL(PpsPreviewPanel::OnMouseWheel)
wxEND_EVENT_TABLE()

namespace {

constexpr double kPi = 3.14159265358979323846;

double DegToRad(double deg) {
    return deg * kPi / 180.0;
}

double ClampUnit(double v) {
    if (v > 1.0) {
        return 1.0;
    }
    if (v < -1.0) {
        return -1.0;
    }
    return v;
}

double ClampRange(double v, double lo, double hi) {
    return std::clamp(v, lo, hi);
}

}  // namespace

PpsFrame::PpsFrame(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, wxT("PPS Demo"), wxDefaultPosition, wxSize(1880, 980)) {
    SetMinSize(wxSize(1500, 900));
    BuildUi();
    BindButtons();
    RefreshAllOutputs();
}

wxTextCtrl* PpsFrame::MakeEntry(wxWindow* parent, bool readonly) {
    long style = wxTE_RIGHT;
    if (readonly) {
        style |= wxTE_READONLY;
    }
    return new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(74, -1), style);
}

wxTextCtrl* PpsFrame::AddLabeledEntry(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, bool readonly) {
    grid->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    wxTextCtrl* entry = MakeEntry(parent, readonly);
    grid->Add(entry, 0, wxRIGHT, 10);
    return entry;
}

void PpsFrame::BuildUi() {
    wxPanel* root = new wxPanel(this);
    wxBoxSizer* root_sizer = new wxBoxSizer(wxHORIZONTAL);

    preview_panel_ = new PpsPreviewPanel(root, this);
    preview_panel_->SetMinSize(wxSize(380, 760));
    root_sizer->Add(preview_panel_, 2, wxALL | wxEXPAND, 10);

    wxScrolledWindow* scroll = new wxScrolledWindow(root, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetScrollRate(0, 20);
    scroll->SetMinSize(wxSize(720, -1));
    wxBoxSizer* right_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* title = new wxStaticText(scroll, wxID_ANY, wxT("Vitus 6D Demo (C++ Port)"));
    wxFont title_font(13, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    right_sizer->Add(title, 0, wxALL, 6);

    auto make_axis_group = [&](const wxString& title) {
        wxStaticBoxSizer* box = new wxStaticBoxSizer(wxVERTICAL, scroll, title);
        wxGridSizer* grid = new wxGridSizer(0, 3, 6, 6);
        box->Add(grid, 1, wxALL | wxEXPAND, 6);
        return std::make_pair(box, grid);
    };
    auto add_step_buttons = [&](wxGridSizer* grid, const wxString& name, wxButton*& plus_btn, wxButton*& minus_btn) {
        grid->Add(new wxStaticText(scroll, wxID_ANY, name), 0, wxALIGN_CENTER_VERTICAL);
        plus_btn = new wxButton(scroll, wxID_ANY, wxT("+"));
        minus_btn = new wxButton(scroll, wxID_ANY, wxT("-"));
        grid->Add(plus_btn, 0, wxEXPAND);
        grid->Add(minus_btn, 0, wxEXPAND);
    };

    auto trans_group = make_axis_group(wxT("Lateral / Longitudinal / Vertical"));
    add_step_buttons(trans_group.second, wxT("Lateral"), btn_lateral_plus_, btn_lateral_minus_);
    add_step_buttons(trans_group.second, wxT("Longitudinal"), btn_longitudinal_plus_, btn_longitudinal_minus_);
    add_step_buttons(trans_group.second, wxT("Vertical"), btn_vertical_plus_, btn_vertical_minus_);

    auto angle_group = make_axis_group(wxT("Pitch / Roll"));
    add_step_buttons(angle_group.second, wxT("Pitch"), btn_pitch_plus_, btn_pitch_minus_);
    add_step_buttons(angle_group.second, wxT("Roll"), btn_roll_plus_, btn_roll_minus_);

    auto iso_group = make_axis_group(wxT("Iso / Column"));
    add_step_buttons(iso_group.second, wxT("Iso"), btn_iso_plus_, btn_iso_minus_);
    add_step_buttons(iso_group.second, wxT("Column"), btn_column_plus_, btn_column_minus_);

    wxBoxSizer* axis_row = new wxBoxSizer(wxHORIZONTAL);
    axis_row->Add(trans_group.first, 1, wxRIGHT | wxEXPAND, 6);
    axis_row->Add(angle_group.first, 1, wxLEFT | wxRIGHT | wxEXPAND, 6);
    axis_row->Add(iso_group.first, 1, wxLEFT | wxEXPAND, 6);
    right_sizer->Add(axis_row, 0, wxALL | wxEXPAND, 6);

    wxBoxSizer* op_buttons = new wxBoxSizer(wxHORIZONTAL);
    wxButton* btn_reset = new wxButton(scroll, wxID_ANY, wxT("Reset All"));
    wxButton* btn_align = new wxButton(scroll, wxID_ANY, wxT("Iso Align"));
    op_buttons->Add(btn_reset, 0, wxRIGHT, 8);
    op_buttons->Add(btn_align, 0, wxRIGHT, 8);
    right_sizer->Add(op_buttons, 0, wxALL, 6);

    auto create_block = [&](const wxString& label, bool add_to_layout = true, int rows = 3, int cols = 8,
                            bool grow_first_value = true) {
        wxStaticBoxSizer* box = new wxStaticBoxSizer(wxVERTICAL, scroll, label);
        wxFlexGridSizer* grid = new wxFlexGridSizer(rows, cols, 6, 6);
        if (cols > 1 && grow_first_value) {
            grid->AddGrowableCol(1);
        }
        box->Add(grid, 0, wxALL | wxEXPAND, 6);
        if (add_to_layout) {
            right_sizer->Add(box, 0, wxALL | wxEXPAND, 6);
        }
        return std::make_pair(box, grid);
    };
    auto add_compact_entry = [&](wxFlexGridSizer* grid, const wxString& label, bool readonly) {
        grid->Add(new wxStaticText(scroll, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
        long style = wxTE_RIGHT;
        if (readonly) {
            style |= wxTE_READONLY;
        }
        wxTextCtrl* entry = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(54, -1), style);
        grid->Add(entry, 0, wxRIGHT, 6);
        return entry;
    };

    auto abs_block = create_block(wxT("PPS Abs Move (PPS Internal)"), false);
    entry_lateral_abs_ = AddLabeledEntry(scroll, abs_block.second, wxT("Lat"), false);
    entry_longitudinal_abs_ = AddLabeledEntry(scroll, abs_block.second, wxT("Long"), false);
    entry_vertical_abs_ = AddLabeledEntry(scroll, abs_block.second, wxT("Vert"), false);
    entry_pitch_abs_ = AddLabeledEntry(scroll, abs_block.second, wxT("Pitch"), false);
    entry_roll_abs_ = AddLabeledEntry(scroll, abs_block.second, wxT("Roll"), false);
    entry_iso_abs_ = AddLabeledEntry(scroll, abs_block.second, wxT("Iso"), false);
    entry_column_abs_ = AddLabeledEntry(scroll, abs_block.second, wxT("Col"), false);
    wxButton* btn_move_abs = new wxButton(scroll, wxID_ANY, wxT("PPS Abs Move"));
    abs_block.first->Add(btn_move_abs, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    auto delta_block = create_block(wxT("PPS Delta Move (PPS Internal)"), false);
    entry_lateral_delta_ = AddLabeledEntry(scroll, delta_block.second, wxT("Lat"), false);
    entry_longitudinal_delta_ = AddLabeledEntry(scroll, delta_block.second, wxT("Long"), false);
    entry_vertical_delta_ = AddLabeledEntry(scroll, delta_block.second, wxT("Vert"), false);
    entry_pitch_delta_ = AddLabeledEntry(scroll, delta_block.second, wxT("Pitch"), false);
    entry_roll_delta_ = AddLabeledEntry(scroll, delta_block.second, wxT("Roll"), false);
    entry_iso_delta_ = AddLabeledEntry(scroll, delta_block.second, wxT("Iso"), false);
    entry_column_delta_ = AddLabeledEntry(scroll, delta_block.second, wxT("Col"), false);
    wxButton* btn_move_delta = new wxButton(scroll, wxID_ANY, wxT("PPS Delta Move"));
    delta_block.first->Add(btn_move_delta, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    wxBoxSizer* pps_move_row = new wxBoxSizer(wxHORIZONTAL);
    pps_move_row->Add(abs_block.first, 1, wxRIGHT | wxEXPAND, 6);
    pps_move_row->Add(delta_block.first, 1, wxLEFT | wxEXPAND, 6);
    right_sizer->Add(pps_move_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    wxButton* btn_resolve_current = new wxButton(scroll, wxID_ANY, wxT("Transform PPS To IEC61217"));
    wxButton* btn_copy_current = new wxButton(scroll, wxID_ANY, wxT("Copy Result To Target"));

    auto target_abs_block = create_block(wxT("Target Abs Move (IEC 61217)"), false);
    entry_61217_lateral_ = AddLabeledEntry(scroll, target_abs_block.second, wxT("Lat"), false);
    entry_61217_longitudinal_ = AddLabeledEntry(scroll, target_abs_block.second, wxT("Long"), false);
    entry_61217_vertical_ = AddLabeledEntry(scroll, target_abs_block.second, wxT("Vert"), false);
    entry_61217_pitch_ = AddLabeledEntry(scroll, target_abs_block.second, wxT("Pitch"), false);
    entry_61217_roll_ = AddLabeledEntry(scroll, target_abs_block.second, wxT("Roll"), false);
    entry_61217_iso_ = AddLabeledEntry(scroll, target_abs_block.second, wxT("Yaw"), false);
    wxButton* btn_move_61217 = new wxButton(scroll, wxID_ANY, wxT("Target Abs Move"));
    target_abs_block.first->Add(btn_move_61217, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    auto corr_block = create_block(wxT("Correction Shift (IEC 61217)"), false);
    entry_61217_lateral_delta_ = AddLabeledEntry(scroll, corr_block.second, wxT("Lat"), false);
    entry_61217_longitudinal_delta_ = AddLabeledEntry(scroll, corr_block.second, wxT("Long"), false);
    entry_61217_vertical_delta_ = AddLabeledEntry(scroll, corr_block.second, wxT("Vert"), false);
    entry_61217_pitch_delta_ = AddLabeledEntry(scroll, corr_block.second, wxT("Pitch"), false);
    entry_61217_roll_delta_ = AddLabeledEntry(scroll, corr_block.second, wxT("Roll"), false);
    entry_61217_iso_delta_ = AddLabeledEntry(scroll, corr_block.second, wxT("Yaw"), false);
    wxButton* btn_corr = new wxButton(scroll, wxID_ANY, wxT("Correction Shift"));
    corr_block.first->Add(btn_corr, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    wxBoxSizer* iec_move_row = new wxBoxSizer(wxHORIZONTAL);
    iec_move_row->Add(target_abs_block.first, 1, wxRIGHT | wxEXPAND, 6);
    iec_move_row->Add(corr_block.first, 1, wxLEFT | wxEXPAND, 6);
    right_sizer->Add(iec_move_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, 6);

    auto target_res_block = create_block(wxT("Target Pos (IEC 61217)"), true, 1, 12, false);
    entry_61217_lateral_res_ = add_compact_entry(target_res_block.second, wxT("Lat"), true);
    entry_61217_longitudinal_res_ = add_compact_entry(target_res_block.second, wxT("Long"), true);
    entry_61217_vertical_res_ = add_compact_entry(target_res_block.second, wxT("Vert"), true);
    entry_61217_pitch_res_ = add_compact_entry(target_res_block.second, wxT("Pitch"), true);
    entry_61217_roll_res_ = add_compact_entry(target_res_block.second, wxT("Roll"), true);
    entry_61217_iso_res_ = add_compact_entry(target_res_block.second, wxT("Yaw"), true);

    auto target_fix_block = create_block(wxT("Target Pos (Fix Coordinate)"), true, 1, 12, false);
    entry_61217_x_res_ = add_compact_entry(target_fix_block.second, wxT("X"), true);
    entry_61217_y_res_ = add_compact_entry(target_fix_block.second, wxT("Y"), true);
    entry_61217_z_res_ = add_compact_entry(target_fix_block.second, wxT("Z"), true);
    entry_61217_rx_res_ = add_compact_entry(target_fix_block.second, wxT("RX"), true);
    entry_61217_ry_res_ = add_compact_entry(target_fix_block.second, wxT("RY"), true);
    entry_61217_rz_res_ = add_compact_entry(target_fix_block.second, wxT("RZ"), true);

    auto target_pps_block = create_block(wxT("Target Pos (PPS Internal)"), true, 1, 12, false);
    entry_61217_lateral_pps_ = add_compact_entry(target_pps_block.second, wxT("Lat"), true);
    entry_61217_longitudinal_pps_ = add_compact_entry(target_pps_block.second, wxT("Long"), true);
    entry_61217_vertical_pps_ = add_compact_entry(target_pps_block.second, wxT("Vert"), true);
    entry_61217_pitch_pps_ = add_compact_entry(target_pps_block.second, wxT("Pitch"), true);
    entry_61217_roll_pps_ = add_compact_entry(target_pps_block.second, wxT("Roll"), true);
    entry_61217_iso_pps_ = add_compact_entry(target_pps_block.second, wxT("Iso"), true);

    wxBoxSizer* target_row = new wxBoxSizer(wxHORIZONTAL);
    wxButton* btn_target_to_pps = new wxButton(scroll, wxID_ANY, wxT("Transform Target To PPS"));
    wxButton* btn_copy_target = new wxButton(scroll, wxID_ANY, wxT("Copy Result To PPS"));
    target_row->Add(btn_target_to_pps, 1, wxRIGHT, 8);
    target_row->Add(btn_copy_target, 1);
    right_sizer->Add(target_row, 0, wxALL | wxEXPAND, 6);

    wxBoxSizer* trans_row = new wxBoxSizer(wxHORIZONTAL);
    trans_row->Add(btn_resolve_current, 1, wxRIGHT, 8);
    trans_row->Add(btn_copy_current, 1);
    right_sizer->Add(trans_row, 0, wxALL | wxEXPAND, 6);

    scroll->SetSizer(right_sizer);
    right_sizer->FitInside(scroll);

    root_sizer->Add(scroll, 3, wxALL | wxEXPAND, 6);
    root->SetSizer(root_sizer);

    btn_reset->Bind(wxEVT_BUTTON, &PpsFrame::OnResetAll, this);
    btn_align->Bind(wxEVT_BUTTON, &PpsFrame::OnIsoAlign, this);
    btn_move_abs->Bind(wxEVT_BUTTON, &PpsFrame::OnMoveTableAbs, this);
    btn_move_delta->Bind(wxEVT_BUTTON, &PpsFrame::OnMoveTableDelta, this);
    btn_resolve_current->Bind(wxEVT_BUTTON, &PpsFrame::OnResolveCurrent61217, this);
    btn_copy_current->Bind(wxEVT_BUTTON, &PpsFrame::OnCopyCurrentToTarget, this);
    btn_move_61217->Bind(wxEVT_BUTTON, &PpsFrame::OnMove61217Abs, this);
    btn_corr->Bind(wxEVT_BUTTON, &PpsFrame::OnMove61217Delta, this);
    btn_target_to_pps->Bind(wxEVT_BUTTON, &PpsFrame::OnResolveTargetToPps, this);
    btn_copy_target->Bind(wxEVT_BUTTON, &PpsFrame::OnCopyTargetToPps, this);
}

void PpsFrame::BindButtons() {
    btn_iso_plus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepIsoPlus, this);
    btn_iso_minus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepIsoMinus, this);
    btn_vertical_plus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepVerticalPlus, this);
    btn_vertical_minus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepVerticalMinus, this);
    btn_pitch_plus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepPitchPlus, this);
    btn_pitch_minus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepPitchMinus, this);
    btn_lateral_plus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepLateralPlus, this);
    btn_lateral_minus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepLateralMinus, this);
    btn_longitudinal_plus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepLongitudinalPlus, this);
    btn_longitudinal_minus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepLongitudinalMinus, this);
    btn_roll_plus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepRollPlus, this);
    btn_roll_minus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepRollMinus, this);
    btn_column_plus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepColumnPlus, this);
    btn_column_minus_->Bind(wxEVT_BUTTON, &PpsFrame::OnStepColumnMinus, this);
}

void PpsFrame::ApplyMotion(const MotionDiff& diff) {
    if (diff.relative) {
        iso_ += diff.iso;
        vertical_ += diff.vertical;
        pitch_ += diff.pitch;
        lateral_ += diff.lateral;
        longitudinal_ += diff.longitudinal;
        roll_ += diff.roll;
        column_ += diff.column;
        elev_ += diff.elev;
        azim_ += diff.azim;
    } else {
        iso_ = diff.iso;
        vertical_ = diff.vertical;
        pitch_ = diff.pitch;
        lateral_ = diff.lateral;
        longitudinal_ = diff.longitudinal;
        roll_ = diff.roll;
        column_ = diff.column;
        elev_ = diff.elev;
        azim_ = diff.azim;
    }

    ClampPpsAxes();
    RefreshAllOutputs();
}

void PpsFrame::ClampPpsAxes() {
    iso_ = ClampRange(iso_, -180.0, 180.0);
    column_ = ClampRange(column_, -180.0, 180.0);
    vertical_ = ClampRange(vertical_, -55.0, 27.0);
    pitch_ = ClampRange(pitch_, -3.0, 3.0);
    roll_ = ClampRange(roll_, -3.0, 3.0);
    lateral_ = ClampRange(lateral_, -25.0, 25.0);
    longitudinal_ = ClampRange(longitudinal_, 0.0, 100.0);
    elev_ = ClampRange(elev_, -88.0, 88.0);
    azim_ = ClampRange(azim_, -180.0, 180.0);
    zoom_scale_ = ClampRange(zoom_scale_, 0.35, 3.0);
}

void PpsFrame::ClampIecAxes() {
    iso_61217_ = ClampRange(iso_61217_, -180.0, 180.0);
    vertical_61217_ = ClampRange(vertical_61217_, -55.0, 27.0);
    pitch_61217_ = ClampRange(pitch_61217_, -3.0, 3.0);
    lateral_61217_ = ClampRange(lateral_61217_, -25.0, 25.0);
    longitudinal_61217_ = ClampRange(longitudinal_61217_, 0.0, 100.0);
    roll_61217_ = ClampRange(roll_61217_, -3.0, 3.0);
}

PpsFrame::Vec3 PpsFrame::IsoTrans(const Vec3& p) const {
    const double a = DegToRad(iso_);
    return {p.x * std::cos(a) - p.y * std::sin(a), p.x * std::sin(a) + p.y * std::cos(a), p.z};
}

PpsFrame::Vec3 PpsFrame::ColumnTrans(const Vec3& p) const {
    const double a = DegToRad(column_);
    Vec3 q = {p.x * std::cos(a) - p.y * std::sin(a), p.x * std::sin(a) + p.y * std::cos(a), p.z};
    q.y += le_distance_;
    return IsoTrans(q);
}

PpsFrame::Vec3 PpsFrame::VerticalTrans(const Vec3& p) const {
    Vec3 q = {p.x, p.y + vty_distance_, p.z + vtz_distance_ + vertical_};
    return ColumnTrans(q);
}

PpsFrame::Vec3 PpsFrame::PitchTrans(const Vec3& p) const {
    const double a = DegToRad(pitch_);
    Vec3 q = {p.x, p.y * std::cos(a) - p.z * std::sin(a), p.y * std::sin(a) + p.z * std::cos(a)};
    q.z += lprz_distance_;
    return VerticalTrans(q);
}

PpsFrame::Vec3 PpsFrame::LateralTrans(const Vec3& p) const {
    Vec3 q = {p.x + lateral_, p.y, p.z + lxt_distance_};
    return PitchTrans(q);
}

PpsFrame::Vec3 PpsFrame::LongitudinalTrans(const Vec3& p) const {
    Vec3 q = {p.x, p.y + longitudinal_ + lyty_distance_, p.z + lytz_distance_};
    return LateralTrans(q);
}

PpsFrame::Vec3 PpsFrame::RollTrans(const Vec3& p) const {
    const double a = DegToRad(roll_);
    Vec3 q = {p.x * std::cos(a) + p.z * std::sin(a), p.y, p.z * std::cos(a) - p.x * std::sin(a)};

    const double guide = DegToRad(linear_guide_angle_);
    const double x_offset =
        (std::sin(guide - a) - std::sin(guide + a)) * linear_guide_distance_ / (4.0 * std::sin(guide));
    const double z_offset =
        (std::sin(guide + a) + std::sin(guide - a) - 2.0 * std::sin(guide)) *
        linear_guide_distance_ / (4.0 * std::cos(guide));

    q.x += x_offset;
    q.z += z_offset + lrrz_distance_;
    return LongitudinalTrans(q);
}

PpsFrame::Vec3 PpsFrame::BallTrans(const Vec3& p) const {
    Vec3 q = {p.x, p.y, p.z + ltz_distance_};
    return RollTrans(q);
}

PpsFrame::Vec3 PpsFrame::Ball61217Trans(const Vec3& p) const {
    Vec3 out = p;
    for (auto it = delta_list_.rbegin(); it != delta_list_.rend(); ++it) {
        const double roll_angle = DegToRad(it->roll);
        const double x1 = out.x * std::cos(roll_angle) + out.z * std::sin(roll_angle);
        const double z1 = out.z * std::cos(roll_angle) - out.x * std::sin(roll_angle);

        const double pitch_angle = DegToRad(it->pitch);
        const double y1 = out.y * std::cos(pitch_angle) - z1 * std::sin(pitch_angle);
        const double z2 = out.y * std::sin(pitch_angle) + z1 * std::cos(pitch_angle);

        const double x2 = x1 + it->lateral;
        const double y2 = y1 + it->longitudinal;
        const double z3 = z2 + it->vertical;

        const double iso_angle = DegToRad(it->iso);
        out.x = x2 * std::cos(iso_angle) - y2 * std::sin(iso_angle);
        out.y = x2 * std::sin(iso_angle) + y2 * std::cos(iso_angle);
        out.z = z3;
    }
    return out;
}

PpsFrame::Vec3 PpsFrame::CorrectionShiftTrans(const Vec3& p) const {
    Vec3 q = {p.x + lateral_correction_shift_, p.y + longitudinal_correction_shift_, p.z + vertical_correction_shift_};

    const double iso_r = DegToRad(iso_correction_shift_);
    const double x2 = q.x * std::cos(iso_r) - q.y * std::sin(iso_r);
    const double y2 = q.x * std::sin(iso_r) + q.y * std::cos(iso_r);

    const double roll_r = DegToRad(roll_correction_shift_);
    const double x3 = x2 * std::cos(roll_r) + q.z * std::sin(roll_r);
    const double z2 = q.z * std::cos(roll_r) - x2 * std::sin(roll_r);

    const double pitch_r = DegToRad(pitch_correction_shift_);
    const double y3 = y2 * std::cos(pitch_r) - z2 * std::sin(pitch_r);
    const double z3 = y2 * std::sin(pitch_r) + z2 * std::cos(pitch_r);
    return {x3, y3, z3};
}

double PpsFrame::CalcAxisAngle(const Vec3& axis) const {
    const Vec3 o = BallTrans({0.0, 0.0, 0.0});
    const Vec3 p = BallTrans(axis);
    const Vec3 v2 = {p.x - o.x, p.y - o.y, p.z - o.z};
    const Vec3 v3 = {
        axis.y * v2.z - axis.z * v2.y,
        axis.z * v2.x - axis.x * v2.z,
        axis.x * v2.y - axis.y * v2.x,
    };
    const double norm = std::sqrt(v3.x * v3.x + v3.y * v3.y + v3.z * v3.z);
    const double sign = (v3.x + v3.y + v3.z) < 0.0 ? -1.0 : 1.0;
    return std::asin(ClampUnit(norm)) / kPi * 180.0 * sign;
}

double PpsFrame::CalcAxisAngle61217(const Vec3& axis) const {
    const Vec3 o = Ball61217Trans({0.0, 0.0, 0.0});
    const Vec3 p = Ball61217Trans(axis);
    const Vec3 v2 = {p.x - o.x, p.y - o.y, p.z - o.z};
    const Vec3 v3 = {
        axis.y * v2.z - axis.z * v2.y,
        axis.z * v2.x - axis.x * v2.z,
        axis.x * v2.y - axis.y * v2.x,
    };
    const double norm = std::sqrt(v3.x * v3.x + v3.y * v3.y + v3.z * v3.z);
    const double sign = (v3.x + v3.y + v3.z) < 0.0 ? -1.0 : 1.0;
    return std::asin(ClampUnit(norm)) / kPi * 180.0 * sign;
}

std::pair<double, double> PpsFrame::ResolveIsoCoordinate(double x, double y, double iso_input_deg) const {
    const double r = DegToRad(iso_input_deg);
    const double c = std::cos(r);
    const double s = std::sin(r);
    // Inverse of [c -s; s c]
    return {c * x + s * y, -s * x + c * y};
}

void PpsFrame::SetValue(wxTextCtrl* entry, double value) {
    if (entry == nullptr) {
        return;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << value;
    entry->ChangeValue(wxString::FromUTF8(oss.str()));
}

double PpsFrame::ParseOrZero(wxTextCtrl* entry) const {
    if (entry == nullptr) {
        return 0.0;
    }
    const wxString text = entry->GetValue().Trim(true).Trim(false);
    if (text.empty()) {
        return 0.0;
    }
    double value = 0.0;
    if (!text.ToDouble(&value)) {
        return 0.0;
    }
    return value;
}

void PpsFrame::RefreshAllOutputs() {
    Freeze();

    const Vec3 p0 = BallTrans({0.0, 0.0, 0.0});
    const auto resolved_cur = ResolveIsoCoordinate(p0.x, p0.y, iso_);
    lateral_offset_p2i_ = resolved_cur.first;
    longitudinal_offset_p2i_ = resolved_cur.second;
    vertical_offset_p2i_ = p0.z;
    roll_offset_p2i_ = roll_;
    pitch_offset_p2i_ = pitch_;
    iso_offset_p2i_ = iso_;

    const Vec3 t0 = Ball61217Trans({0.0, 0.0, 0.0});
    const Vec3 t1 = Ball61217Trans({1.0, 0.0, 0.0});
    const Vec3 t2 = Ball61217Trans({0.0, 1.0, 0.0});

    const double m21 = t1.y - t0.y;
    const double m31 = t1.z - t0.z;
    const double m32 = t2.z - t0.z;

    const double roll_r = std::asin(ClampUnit(-m31));
    const double pitch_r = std::asin(ClampUnit(m32 / std::max(1e-9, std::cos(roll_r))));
    const double iso_r = std::asin(ClampUnit(m21 / std::max(1e-9, std::cos(roll_r))));

    SetValue(entry_61217_x_res_, t0.x);
    SetValue(entry_61217_y_res_, t0.y);
    SetValue(entry_61217_z_res_, t0.z);
    SetValue(entry_61217_rx_res_, pitch_r / kPi * 180.0);
    SetValue(entry_61217_ry_res_, roll_r / kPi * 180.0);
    SetValue(entry_61217_rz_res_, iso_r / kPi * 180.0);

    SetValue(entry_61217_lateral_res_, lateral_61217_);
    SetValue(entry_61217_longitudinal_res_, longitudinal_61217_);
    SetValue(entry_61217_vertical_res_, vertical_61217_);
    SetValue(entry_61217_pitch_res_, pitch_61217_);
    SetValue(entry_61217_roll_res_, roll_61217_);
    SetValue(entry_61217_iso_res_, iso_61217_);

    SetValue(entry_61217_lateral_pps_, lateral_offset_i2p_);
    SetValue(entry_61217_longitudinal_pps_, longitudinal_offset_i2p_);
    SetValue(entry_61217_vertical_pps_, vertical_offset_i2p_);
    SetValue(entry_61217_pitch_pps_, pitch_offset_i2p_);
    SetValue(entry_61217_roll_pps_, roll_offset_i2p_);
    SetValue(entry_61217_iso_pps_, iso_offset_i2p_);

    if (preview_panel_ != nullptr) {
        preview_panel_->Refresh(false);
    }

    Thaw();
}

void PpsFrame::RenderPreview(wxDC& dc, const wxSize& size) {
    dc.SetBackground(wxBrush(wxColour(245, 247, 250)));
    dc.Clear();

    const double cx = size.GetWidth() * 0.5;
    const double cy = size.GetHeight() * 0.52;
    const double focal = 600.0 * zoom_scale_;

    auto camera_rotate = [&](const Vec3& p) {
        const double az = DegToRad(azim_);
        const double el = DegToRad(elev_);
        const double x1 = p.x * std::cos(az) - p.y * std::sin(az);
        const double y1 = p.x * std::sin(az) + p.y * std::cos(az);
        const double z1 = p.z;

        const double y2 = y1 * std::cos(el) - z1 * std::sin(el);
        const double z2 = y1 * std::sin(el) + z1 * std::cos(el);
        return Vec3{x1, y2, z2};
    };

    auto project = [&](const Vec3& p) {
        const Vec3 c = camera_rotate(p);
        const double depth = std::max(80.0, focal + c.z);
        const double sx = cx + c.x * focal / depth;
        const double sy = cy - c.y * focal / depth;
        return wxPoint(static_cast<int>(sx), static_cast<int>(sy));
    };

    auto draw_line = [&](const Vec3& a, const Vec3& b, const wxColour& color, int width = 2) {
        dc.SetPen(wxPen(color, width));
        dc.DrawLine(project(a), project(b));
    };

    auto draw_axis_arrow = [&](const Vec3& a, const Vec3& b, const wxColour& color, int width = 3) {
        draw_line(a, b, color, width);
        const wxPoint p0 = project(a);
        const wxPoint p1 = project(b);
        const double vx = static_cast<double>(p1.x - p0.x);
        const double vy = static_cast<double>(p1.y - p0.y);
        const double len = std::sqrt(vx * vx + vy * vy);
        if (len < 1.0) {
            return;
        }

        const double ux = vx / len;
        const double uy = vy / len;
        const double head_len = 10.0 + width;
        const double wing = 5.0 + width * 0.5;

        const wxPoint left(
            static_cast<int>(p1.x - head_len * ux + wing * uy),
            static_cast<int>(p1.y - head_len * uy - wing * ux));
        const wxPoint right(
            static_cast<int>(p1.x - head_len * ux - wing * uy),
            static_cast<int>(p1.y - head_len * uy + wing * ux));

        dc.SetPen(wxPen(color, width));
        dc.DrawLine(p1, left);
        dc.DrawLine(p1, right);
    };

    auto depth = [&](const Vec3& p) {
        return camera_rotate(p).z;
    };

    auto draw_cross = [&](const Vec3& c, const wxColour& color) {
        const wxPoint p = project(c);
        dc.SetPen(wxPen(color, 2));
        dc.DrawLine(p.x - 5, p.y, p.x + 5, p.y);
        dc.DrawLine(p.x, p.y - 5, p.x, p.y + 5);
    };

    struct Face {
        std::array<Vec3, 4> pts;
        double z = 0.0;
        wxColour color;
    };

    auto shade = [](const wxColour& c, double factor) {
        const int r = static_cast<int>(std::clamp(c.Red() * factor, 0.0, 255.0));
        const int g = static_cast<int>(std::clamp(c.Green() * factor, 0.0, 255.0));
        const int b = static_cast<int>(std::clamp(c.Blue() * factor, 0.0, 255.0));
        return wxColour(r, g, b);
    };

    auto draw_solid_box = [&](const Vec3& half, const Vec3& offset, const wxColour& base,
                              const std::function<Vec3(const Vec3&)>& transform) {
        std::array<Vec3, 8> v = {
            Vec3{-half.x, -half.y, -half.z}, Vec3{half.x, -half.y, -half.z},
            Vec3{half.x, half.y, -half.z},   Vec3{-half.x, half.y, -half.z},
            Vec3{-half.x, -half.y, half.z},  Vec3{half.x, -half.y, half.z},
            Vec3{half.x, half.y, half.z},    Vec3{-half.x, half.y, half.z}};

        for (auto& p : v) {
            p.x += offset.x;
            p.y += offset.y;
            p.z += offset.z;
            p = transform(p);
        }

        std::array<std::array<int, 4>, 6> idx = {{{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                                                   {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}}};
        std::array<double, 6> shades = {0.82, 1.10, 0.92, 0.76, 0.88, 0.70};

        std::vector<Face> faces;
        faces.reserve(6);
        for (size_t i = 0; i < idx.size(); ++i) {
            Face f;
            f.pts = {v[idx[i][0]], v[idx[i][1]], v[idx[i][2]], v[idx[i][3]]};
            f.z = 0.25 * (depth(f.pts[0]) + depth(f.pts[1]) + depth(f.pts[2]) + depth(f.pts[3]));
            f.color = shade(base, shades[i]);
            faces.push_back(f);
        }

        std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.z < b.z; });

        for (const auto& f : faces) {
            wxPoint pts[4] = {project(f.pts[0]), project(f.pts[1]), project(f.pts[2]), project(f.pts[3])};
            dc.SetPen(wxPen(shade(f.color, 0.72), 1));
            dc.SetBrush(wxBrush(f.color));
            dc.DrawPolygon(4, pts);
        }
    };

    auto draw_curved_surface = [&](int nu, int nv,
                                   const std::function<Vec3(double, double)>& local_surface,
                                   const std::function<Vec3(const Vec3&)>& transform,
                                   const wxColour& color0,
                                   const wxColour& color1) {
        struct Quad {
            Vec3 p[4];
            double z = 0.0;
            wxColour c;
        };
        auto lerp = [](const wxColour& a, const wxColour& b, double t) {
            const int r = static_cast<int>(a.Red() * (1.0 - t) + b.Red() * t);
            const int g = static_cast<int>(a.Green() * (1.0 - t) + b.Green() * t);
            const int bl = static_cast<int>(a.Blue() * (1.0 - t) + b.Blue() * t);
            return wxColour(r, g, bl);
        };

        std::vector<Quad> quads;
        quads.reserve((nu - 1) * (nv - 1));
        for (int i = 0; i < nu - 1; ++i) {
            const double u0 = static_cast<double>(i) / (nu - 1);
            const double u1 = static_cast<double>(i + 1) / (nu - 1);
            for (int j = 0; j < nv - 1; ++j) {
                const double v0 = static_cast<double>(j) / (nv - 1);
                const double v1 = static_cast<double>(j + 1) / (nv - 1);
                Quad q;
                q.p[0] = transform(local_surface(u0, v0));
                q.p[1] = transform(local_surface(u1, v0));
                q.p[2] = transform(local_surface(u1, v1));
                q.p[3] = transform(local_surface(u0, v1));
                q.z = 0.25 * (depth(q.p[0]) + depth(q.p[1]) + depth(q.p[2]) + depth(q.p[3]));
                q.c = lerp(color0, color1, v0);
                quads.push_back(q);
            }
        }

        std::sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) { return a.z < b.z; });
        for (const auto& q : quads) {
            wxPoint pts[4] = {project(q.p[0]), project(q.p[1]), project(q.p[2]), project(q.p[3])};
            dc.SetPen(wxPen(shade(q.c, 0.7), 1));
            dc.SetBrush(wxBrush(q.c));
            dc.DrawPolygon(4, pts);
        }
    };

    auto draw_ball = [&](const Vec3& center, double radius, const wxColour& color) {
        const Vec3 c = camera_rotate(center);
        const double dep = std::max(80.0, focal + c.z);
        const int rr = static_cast<int>(std::max(2.0, radius * focal / dep));
        const wxPoint cp = project(center);
        dc.SetPen(wxPen(shade(color, 0.65), 1));
        dc.SetBrush(wxBrush(color));
        dc.DrawCircle(cp, rr);
    };

    // Fixed grid: same height as turntable plane, but does not follow any axis motion.
    const double grid_z = vtz_distance_ - 2.5;
    const double grid_half = 200.0;
    for (int i = -200; i <= 200; i += 20) {
        Vec3 ga = {-grid_half, static_cast<double>(i), grid_z};
        Vec3 gb = {grid_half, static_cast<double>(i), grid_z};
        Vec3 gc = {static_cast<double>(i), -grid_half, grid_z};
        Vec3 gd = {static_cast<double>(i), grid_half, grid_z};
        draw_line(ga, gb, wxColour(220, 224, 230), 1);
        draw_line(gc, gd, wxColour(220, 224, 230), 1);
    }

    // Solid parts approximating the original matplotlib demo style.
    // Keep this column telescopic with a fixed lower end (iso plane side) and upper end attached to orange assembly.
    const double vertical_span = std::max(10.0, lprz_distance_ + vertical_);
    draw_solid_box(
        {20.0, 50.0, vertical_span * 0.5}, {0.0, 0.0, vertical_span * 0.5}, wxColour(70, 160, 120),
        [&](const Vec3& p) {
            Vec3 q = p;
            q.y += vty_distance_;
            q.z += vtz_distance_;
            return ColumnTrans(q);
        });
    draw_solid_box({30.0, 60.0, 2.5}, {0.0, 0.0, 2.5}, wxColour(235, 125, 55),
                   [&](const Vec3& p) { return RollTrans(p); });
    draw_solid_box({30.0, 60.0, 2.5}, {0.0, 0.0, 2.5}, wxColour(240, 145, 55),
                   [&](const Vec3& p) { return RollTrans(p); });
    draw_solid_box({12.0, 70.0, 4.5}, {-24.0, 0.0, 4.5}, wxColour(130, 210, 135),
                   [&](const Vec3& p) { return RollTrans(p); });
    draw_solid_box({12.0, 70.0, 4.5}, {24.0, 0.0, 4.5}, wxColour(130, 210, 135),
                   [&](const Vec3& p) { return RollTrans(p); });
    draw_solid_box({8.0, 75.0, 8.0}, {0.0, 0.0, 8.0}, wxColour(140, 140, 145),
                   [&](const Vec3& p) { return RollTrans(p); });

    // Add denser curved patches to mimic matplotlib surface texture.
    draw_curved_surface(
        44, 28,
        [&](double u, double v) {
            const double theta = 2.0 * kPi * u;
            const double r = 40.0 * v;
            return Vec3{r * std::cos(theta), r * std::sin(theta), -2.5};
        },
        [&](const Vec3& p) {
            Vec3 q = ColumnTrans(p);
            q.z += vtz_distance_;
            return q;
        },
        wxColour(120, 120, 125), wxColour(185, 185, 190));

    // Turntable rim highlight/shadow layering (matplotlib-like ring impression).
    draw_curved_surface(
        56, 12,
        [&](double u, double v) {
            const double theta = 2.0 * kPi * u;
            const double r = 32.0 + 8.0 * v;
            return Vec3{r * std::cos(theta), r * std::sin(theta), -2.0 + 0.8 * v};
        },
        [&](const Vec3& p) {
            Vec3 q = ColumnTrans(p);
            q.z += vtz_distance_;
            return q;
        },
        wxColour(225, 225, 228), wxColour(95, 95, 102));

    draw_curved_surface(
        44, 18,
        [&](double u, double v) {
            const double x = -30.0 + 60.0 * u;
            const double theta = kPi + kPi * v;
            const double y = 10.0 * std::cos(theta);
            const double z = 10.0 * std::sin(theta);
            return Vec3{x, y, z};
        },
        [&](const Vec3& p) { return RollTrans(p); },
        wxColour(255, 168, 88), wxColour(255, 123, 48));

    draw_curved_surface(
        46, 20,
        [&](double u, double v) {
            const double y = -75.0 + 150.0 * u;
            const double theta = kPi + kPi * v;
            const double x = 8.8 * std::cos(theta);
            const double z = 8.8 * std::sin(theta) + 8.8;
            return Vec3{x, y, z};
        },
        [&](const Vec3& p) {
            Vec3 q = RollTrans(p);
            q.y -= lyty_distance_;
            return q;
        },
        wxColour(170, 170, 176), wxColour(108, 108, 114));

    // Bridge highlight/shadow strips.
    draw_solid_box({11.0, 70.0, 1.0}, {-24.0, 0.0, 9.0}, wxColour(176, 236, 178),
                   [&](const Vec3& p) { return RollTrans(p); });
    draw_solid_box({11.0, 70.0, 1.0}, {24.0, 0.0, 9.0}, wxColour(176, 236, 178),
                   [&](const Vec3& p) { return RollTrans(p); });
    draw_solid_box({11.0, 70.0, 1.0}, {-24.0, 0.0, 1.0}, wxColour(76, 146, 84),
                   [&](const Vec3& p) { return RollTrans(p); });
    draw_solid_box({11.0, 70.0, 1.0}, {24.0, 0.0, 1.0}, wxColour(76, 146, 84),
                   [&](const Vec3& p) { return RollTrans(p); });

    const Vec3 iec_o = Ball61217Trans({0.0, 0.0, 0.0});
    // Fixed coordinate indicator at one grid corner:
    // X -> lateral, Y -> longitudinal, Z -> vertical.
    const Vec3 axis_o = {-180.0, -180.0, grid_z};
    const Vec3 axis_x = {-135.0, -180.0, grid_z};
    const Vec3 axis_y = {-180.0, -135.0, grid_z};
    const Vec3 axis_z = {-180.0, -180.0, grid_z + 45.0};
    draw_axis_arrow(axis_o, axis_x, wxColour(220, 30, 30), 3);
    draw_axis_arrow(axis_o, axis_y, wxColour(220, 200, 30), 3);
    draw_axis_arrow(axis_o, axis_z, wxColour(40, 90, 220), 3);

    const wxPoint x_tip = project(axis_x);
    const wxPoint y_tip = project(axis_y);
    const wxPoint z_tip = project(axis_z);
    dc.SetTextForeground(wxColour(190, 35, 35));
    dc.DrawText(wxT("X"), x_tip.x + 6, x_tip.y - 6);
    dc.SetTextForeground(wxColour(175, 150, 35));
    dc.DrawText(wxT("Y"), y_tip.x + 6, y_tip.y - 6);
    dc.SetTextForeground(wxColour(35, 80, 190));
    dc.DrawText(wxT("Z"), z_tip.x + 6, z_tip.y - 6);

    draw_ball(iec_o, 5.0, wxColour(215, 70, 70));
    draw_cross(axis_o, wxColour(25, 25, 25));
    draw_cross(iec_o, wxColour(85, 85, 85));

    dc.SetTextForeground(wxColour(40, 40, 50));
    dc.DrawText(wxString::Format(wxT("Elev %.1f, Azim %.1f, Zoom %.2f"), elev_, azim_, zoom_scale_), 12, 12);
    dc.DrawText(wxT("Left drag: rotate. Mouse wheel: zoom in/out."), 12, 32);
    dc.DrawText(wxT("Axis mapping: X=lateral, Y=longitudinal, Z=vertical"), 12, 52);

    const int overlay_w = 292;
    const int overlay_h = 326;
    const int overlay_x = std::max(10, size.GetWidth() - overlay_w - 12);
    const int overlay_y = 12;
    dc.SetPen(wxPen(wxColour(150, 156, 168), 1));
    dc.SetBrush(wxBrush(wxColour(252, 253, 255)));
    dc.DrawRectangle(overlay_x, overlay_y, overlay_w, overlay_h);

    dc.SetTextForeground(wxColour(26, 32, 42));
    dc.DrawText(wxT("Current Axes (PPS)"), overlay_x + 10, overlay_y + 8);
    dc.SetTextForeground(wxColour(58, 66, 82));
    dc.DrawText(wxString::Format(wxT("Lat:  %.2f"), lateral_), overlay_x + 10, overlay_y + 32);
    dc.DrawText(wxString::Format(wxT("Long: %.2f"), longitudinal_), overlay_x + 10, overlay_y + 52);
    dc.DrawText(wxString::Format(wxT("Vert: %.2f"), vertical_), overlay_x + 10, overlay_y + 72);
    dc.DrawText(wxString::Format(wxT("Pitch: %.2f"), pitch_), overlay_x + 10, overlay_y + 92);
    dc.DrawText(wxString::Format(wxT("Roll:  %.2f"), roll_), overlay_x + 10, overlay_y + 112);
    dc.DrawText(wxString::Format(wxT("Iso:   %.2f"), iso_), overlay_x + 10, overlay_y + 132);
    dc.DrawText(wxString::Format(wxT("Col:   %.2f"), column_), overlay_x + 10, overlay_y + 152);

    dc.SetPen(wxPen(wxColour(214, 220, 228), 1));
    dc.DrawLine(overlay_x + 8, overlay_y + 178, overlay_x + overlay_w - 8, overlay_y + 178);
    dc.SetTextForeground(wxColour(26, 32, 42));
    dc.DrawText(wxT("Current Pos (IEC 61217)"), overlay_x + 10, overlay_y + 186);
    dc.SetTextForeground(wxColour(58, 66, 82));
    dc.DrawText(wxString::Format(wxT("Lat:  %.2f"), lateral_offset_p2i_), overlay_x + 10, overlay_y + 210);
    dc.DrawText(wxString::Format(wxT("Long: %.2f"), longitudinal_offset_p2i_), overlay_x + 10, overlay_y + 230);
    dc.DrawText(wxString::Format(wxT("Vert: %.2f"), vertical_offset_p2i_), overlay_x + 10, overlay_y + 250);
    dc.DrawText(wxString::Format(wxT("Pitch: %.2f"), pitch_offset_p2i_), overlay_x + 10, overlay_y + 270);
    dc.DrawText(wxString::Format(wxT("Roll:  %.2f"), roll_offset_p2i_), overlay_x + 10, overlay_y + 290);
    dc.DrawText(wxString::Format(wxT("Yaw:   %.2f"), iso_offset_p2i_), overlay_x + 10, overlay_y + 310);
}

void PpsFrame::OnPaintPreview(wxPaintEvent& event) {
    (void)event;
}

void PpsFrame::OnStepIsoPlus(wxCommandEvent& event) { (void)event; ApplyMotion({10.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepIsoMinus(wxCommandEvent& event) { (void)event; ApplyMotion({-10.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepVerticalPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 5.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepVerticalMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, -5.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepPitchPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepPitchMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepLateralPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 5.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepLateralMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, -5.0, 0.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepLongitudinalPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepLongitudinalMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, -10.0, 0.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepRollPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepRollMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepColumnPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 10.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepColumnMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -10.0, 0.0, 0.0, true}); }
void PpsFrame::OnStepElevPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 15.0, 0.0, true}); }
void PpsFrame::OnStepElevMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -15.0, 0.0, true}); }
void PpsFrame::OnStepAzimPlus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 15.0, true}); }
void PpsFrame::OnStepAzimMinus(wxCommandEvent& event) { (void)event; ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -15.0, true}); }

void PpsFrame::OnResetAll(wxCommandEvent& event) {
    (void)event;
    iso_61217_ = 0.0;
    vertical_61217_ = 0.0;
    pitch_61217_ = 0.0;
    lateral_61217_ = 0.0;
    longitudinal_61217_ = 0.0;
    roll_61217_ = 0.0;
    delta_list_.clear();
    zoom_scale_ = 1.0;
    ApplyMotion({0.0, 0.0, 0.0, 0.0, 50.0, 0.0, 0.0, -90.0, -90.0, false});
}

void PpsFrame::OnIsoAlign(wxCommandEvent& event) {
    (void)event;
    ApplyMotion({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, -90.0, -90.0, false});
}

void PpsFrame::OnMoveTableAbs(wxCommandEvent& event) {
    (void)event;
    iso_ = ParseOrZero(entry_iso_abs_);
    pitch_ = ParseOrZero(entry_pitch_abs_);
    roll_ = ParseOrZero(entry_roll_abs_);
    lateral_ = ParseOrZero(entry_lateral_abs_);
    longitudinal_ = ParseOrZero(entry_longitudinal_abs_);
    vertical_ = ParseOrZero(entry_vertical_abs_);
    column_ = ParseOrZero(entry_column_abs_);
    ApplyMotion({iso_, vertical_, pitch_, lateral_, longitudinal_, roll_, column_, elev_, azim_, false});
}

void PpsFrame::OnMoveTableDelta(wxCommandEvent& event) {
    (void)event;
    iso_ += ParseOrZero(entry_iso_delta_);
    pitch_ += ParseOrZero(entry_pitch_delta_);
    roll_ += ParseOrZero(entry_roll_delta_);
    lateral_ += ParseOrZero(entry_lateral_delta_);
    longitudinal_ += ParseOrZero(entry_longitudinal_delta_);
    vertical_ += ParseOrZero(entry_vertical_delta_);
    column_ += ParseOrZero(entry_column_delta_);
    ClampPpsAxes();
    RefreshAllOutputs();
}

void PpsFrame::OnResolveCurrent61217(wxCommandEvent& event) {
    (void)event;
    const Vec3 p = BallTrans({0.0, 0.0, 0.0});
    const auto resolved = ResolveIsoCoordinate(p.x, p.y, iso_);
    lateral_offset_p2i_ = resolved.first;
    longitudinal_offset_p2i_ = resolved.second;
    vertical_offset_p2i_ = p.z;
    roll_offset_p2i_ = roll_;
    pitch_offset_p2i_ = pitch_;
    iso_offset_p2i_ = iso_;

    RefreshAllOutputs();
}

void PpsFrame::OnCopyCurrentToTarget(wxCommandEvent& event) {
    (void)event;
    SetValue(entry_61217_lateral_, lateral_offset_p2i_);
    SetValue(entry_61217_longitudinal_, longitudinal_offset_p2i_);
    SetValue(entry_61217_vertical_, vertical_offset_p2i_);
    SetValue(entry_61217_pitch_, pitch_offset_p2i_);
    SetValue(entry_61217_roll_, roll_offset_p2i_);
    SetValue(entry_61217_iso_, iso_offset_p2i_);
}

void PpsFrame::OnMove61217Abs(wxCommandEvent& event) {
    (void)event;
    iso_61217_ = ParseOrZero(entry_61217_iso_);
    pitch_61217_ = ParseOrZero(entry_61217_pitch_);
    roll_61217_ = ParseOrZero(entry_61217_roll_);
    lateral_61217_ = ParseOrZero(entry_61217_lateral_);
    longitudinal_61217_ = ParseOrZero(entry_61217_longitudinal_);
    vertical_61217_ = ParseOrZero(entry_61217_vertical_);
    ClampIecAxes();

    delta_list_.clear();
    delta_list_.push_back({lateral_61217_, longitudinal_61217_, vertical_61217_, pitch_61217_, roll_61217_, iso_61217_});

    // Treat IEC61217 absolute input as the motion target: resolve to PPS internal and move the table.
    OnResolveTargetToPps(event);
    ApplyMotion({
        iso_offset_i2p_,
        vertical_offset_i2p_,
        pitch_offset_i2p_,
        lateral_offset_i2p_,
        longitudinal_offset_i2p_,
        roll_offset_i2p_,
        column_,
        elev_,
        azim_,
        false,
    });
}

void PpsFrame::OnMove61217Delta(wxCommandEvent& event) {
    (void)event;
    iso_correction_shift_ = ParseOrZero(entry_61217_iso_delta_);
    pitch_correction_shift_ = ParseOrZero(entry_61217_pitch_delta_);
    roll_correction_shift_ = ParseOrZero(entry_61217_roll_delta_);
    lateral_correction_shift_ = ParseOrZero(entry_61217_lateral_delta_);
    longitudinal_correction_shift_ = ParseOrZero(entry_61217_longitudinal_delta_);
    vertical_correction_shift_ = ParseOrZero(entry_61217_vertical_delta_);

    const Vec3 t0 = Ball61217Trans({0.0, 0.0, 0.0});
    const Vec3 t1 = Ball61217Trans({1.0, 0.0, 0.0});
    const Vec3 t2 = Ball61217Trans({0.0, 1.0, 0.0});

    const Vec3 s0 = CorrectionShiftTrans(t0);
    const Vec3 s1 = CorrectionShiftTrans(t1);
    const Vec3 s2 = CorrectionShiftTrans(t2);

    const double m31 = s1.z - s0.z;
    const double m12 = s2.x - s0.x;
    const double m32 = s2.z - s0.z;

    const double pitch_r = std::asin(ClampUnit(m32));
    const double roll_r = std::asin(ClampUnit((-m31) / std::max(1e-9, std::cos(pitch_r))));
    const double iso_r = std::asin(ClampUnit((-m12) / std::max(1e-9, std::cos(pitch_r))));

    pitch_61217_ = pitch_r / kPi * 180.0;
    roll_61217_ = roll_r / kPi * 180.0;
    iso_61217_ = iso_r / kPi * 180.0;

    const double c = std::cos(iso_r);
    const double s = std::sin(iso_r);
    lateral_61217_ = c * s0.x + s * s0.y;
    longitudinal_61217_ = -s * s0.x + c * s0.y;
    vertical_61217_ = s0.z;
    ClampIecAxes();

    delta_list_.clear();
    delta_list_.push_back({lateral_61217_, longitudinal_61217_, vertical_61217_, pitch_61217_, roll_61217_, iso_61217_});
    RefreshAllOutputs();
}

void PpsFrame::OnResolveTargetToPps(wxCommandEvent& event) {
    (void)event;
    if (delta_list_.empty()) {
        return;
    }

    lateral_offset_i2p_ = delta_list_.front().lateral;
    longitudinal_offset_i2p_ = delta_list_.front().longitudinal;
    vertical_offset_i2p_ = delta_list_.front().vertical;
    pitch_offset_i2p_ = delta_list_.front().pitch;
    roll_offset_i2p_ = delta_list_.front().roll;
    iso_offset_i2p_ = delta_list_.front().iso;

    const Vec3 t = Ball61217Trans({0.0, 0.0, 0.0});
    const auto resolved = ResolveIsoCoordinate(t.x, t.y, iso_offset_i2p_);
    const double tx = resolved.first;
    const double ty = resolved.second;
    const double tz = t.z;

    const double z3 = ltz_distance_;
    const double roll_r = DegToRad(roll_offset_i2p_);
    const double guide = DegToRad(linear_guide_angle_);
    const double rx =
        (std::sin(guide - roll_r) - std::sin(guide + roll_r)) * linear_guide_distance_ / (4.0 * std::sin(guide));
    const double rz =
        (std::sin(guide + roll_r) + std::sin(guide - roll_r) - 2.0 * std::sin(guide)) *
        linear_guide_distance_ / (4.0 * std::cos(guide));

    lateral_offset_i2p_ = tx - rx - z3 * std::sin(roll_r);

    const double y1 = le_distance_ + vty_distance_;
    const double y2 = lyty_distance_;
    const double z2 = lxt_distance_ + lytz_distance_ + lrrz_distance_;
    const double pitch_r = DegToRad(pitch_offset_i2p_);

    longitudinal_offset_i2p_ =
        (ty - y1 + z3 * std::sin(pitch_r) * std::cos(roll_r) + std::sin(pitch_r) * (rz + z2)) /
            std::max(1e-9, std::cos(pitch_r)) -
        y2;

    const double z1 = vtz_distance_ + lprz_distance_;
    vertical_offset_i2p_ =
        tz - z1 - z3 * std::cos(pitch_r) * std::cos(roll_r) - std::cos(pitch_r) * (rz + z2) -
        std::sin(pitch_r) * (longitudinal_offset_i2p_ + y2);

    RefreshAllOutputs();
}

void PpsFrame::OnCopyTargetToPps(wxCommandEvent& event) {
    (void)event;
    entry_lateral_abs_->ChangeValue(entry_61217_lateral_pps_->GetValue());
    entry_longitudinal_abs_->ChangeValue(entry_61217_longitudinal_pps_->GetValue());
    entry_vertical_abs_->ChangeValue(entry_61217_vertical_pps_->GetValue());
    entry_pitch_abs_->ChangeValue(entry_61217_pitch_pps_->GetValue());
    entry_roll_abs_->ChangeValue(entry_61217_roll_pps_->GetValue());
    entry_iso_abs_->ChangeValue(entry_61217_iso_pps_->GetValue());
}
