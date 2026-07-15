#pragma once

#include <utility>
#include <vector>

#include <wx/wx.h>

class PpsPreviewPanel;

class PpsFrame : public wxFrame {
public:
    explicit PpsFrame(wxWindow* parent);

private:
    friend class PpsPreviewPanel;

    struct DeltaAxes {
        double lateral = 0.0;
        double longitudinal = 0.0;
        double vertical = 0.0;
        double pitch = 0.0;
        double roll = 0.0;
        double iso = 0.0;
    };

    struct Vec3 {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    struct MotionDiff {
        double iso = 0.0;
        double vertical = 0.0;
        double pitch = 0.0;
        double lateral = 0.0;
        double longitudinal = 0.0;
        double roll = 0.0;
        double column = 0.0;
        double elev = 0.0;
        double azim = 0.0;
        bool relative = true;
    };

    void BuildUi();
    wxTextCtrl* MakeEntry(wxWindow* parent, bool readonly = false);
    wxTextCtrl* AddLabeledEntry(wxWindow* parent, wxFlexGridSizer* grid, const wxString& label, bool readonly = false);

    void BindButtons();

    void ApplyMotion(const MotionDiff& diff);
    void RefreshAllOutputs();
    void RenderPreview(wxDC& dc, const wxSize& size);
    void ClampPpsAxes();
    void ClampIecAxes();

    Vec3 IsoTrans(const Vec3& p) const;
    Vec3 ColumnTrans(const Vec3& p) const;
    Vec3 VerticalTrans(const Vec3& p) const;
    Vec3 PitchTrans(const Vec3& p) const;
    Vec3 LateralTrans(const Vec3& p) const;
    Vec3 LongitudinalTrans(const Vec3& p) const;
    Vec3 RollTrans(const Vec3& p) const;
    Vec3 BallTrans(const Vec3& p) const;
    Vec3 Ball61217Trans(const Vec3& p) const;
    Vec3 CorrectionShiftTrans(const Vec3& p) const;

    double CalcAxisAngle(const Vec3& axis) const;
    double CalcAxisAngle61217(const Vec3& axis) const;

    std::pair<double, double> ResolveIsoCoordinate(double x, double y, double iso_input_deg) const;

    void OnPaintPreview(wxPaintEvent& event);

    void OnStepIsoPlus(wxCommandEvent& event);
    void OnStepIsoMinus(wxCommandEvent& event);
    void OnStepVerticalPlus(wxCommandEvent& event);
    void OnStepVerticalMinus(wxCommandEvent& event);
    void OnStepPitchPlus(wxCommandEvent& event);
    void OnStepPitchMinus(wxCommandEvent& event);
    void OnStepLateralPlus(wxCommandEvent& event);
    void OnStepLateralMinus(wxCommandEvent& event);
    void OnStepLongitudinalPlus(wxCommandEvent& event);
    void OnStepLongitudinalMinus(wxCommandEvent& event);
    void OnStepRollPlus(wxCommandEvent& event);
    void OnStepRollMinus(wxCommandEvent& event);
    void OnStepColumnPlus(wxCommandEvent& event);
    void OnStepColumnMinus(wxCommandEvent& event);
    void OnStepElevPlus(wxCommandEvent& event);
    void OnStepElevMinus(wxCommandEvent& event);
    void OnStepAzimPlus(wxCommandEvent& event);
    void OnStepAzimMinus(wxCommandEvent& event);

    void OnResetAll(wxCommandEvent& event);
    void OnIsoAlign(wxCommandEvent& event);
    void OnMoveTableAbs(wxCommandEvent& event);
    void OnMoveTableDelta(wxCommandEvent& event);
    void OnResolveCurrent61217(wxCommandEvent& event);
    void OnCopyCurrentToTarget(wxCommandEvent& event);
    void OnMove61217Abs(wxCommandEvent& event);
    void OnMove61217Delta(wxCommandEvent& event);
    void OnResolveTargetToPps(wxCommandEvent& event);
    void OnCopyTargetToPps(wxCommandEvent& event);

    double ParseOrZero(wxTextCtrl* entry) const;
    void SetValue(wxTextCtrl* entry, double value);

    PpsPreviewPanel* preview_panel_ = nullptr;

    double le_distance_ = -100.0;
    double vty_distance_ = 20.9;
    double vtz_distance_ = -108.2;
    double lprz_distance_ = 83.45;
    double lxt_distance_ = 4.05;
    double lyty_distance_ = 79.1;
    double lytz_distance_ = 10.8;
    double lrrz_distance_ = 1.124;
    double ltz_distance_ = 8.8;
    double linear_guide_angle_ = 30.0;
    double linear_guide_distance_ = 28.6;

    double iso_ = 0.0;
    double column_ = 0.0;
    double vertical_ = 0.0;
    double pitch_ = 0.0;
    double lateral_ = 0.0;
    double longitudinal_ = 50.0;
    double roll_ = 0.0;
    double elev_ = -90.0;
    double azim_ = -90.0;
    double zoom_scale_ = 1.0;

    double iso_offset_p2i_ = 0.0;
    double vertical_offset_p2i_ = 0.0;
    double pitch_offset_p2i_ = 0.0;
    double lateral_offset_p2i_ = 0.0;
    double longitudinal_offset_p2i_ = 0.0;
    double roll_offset_p2i_ = 0.0;

    double iso_61217_ = 0.0;
    double vertical_61217_ = 0.0;
    double pitch_61217_ = 0.0;
    double lateral_61217_ = 0.0;
    double longitudinal_61217_ = 0.0;
    double roll_61217_ = 0.0;

    double iso_offset_i2p_ = 0.0;
    double vertical_offset_i2p_ = 0.0;
    double pitch_offset_i2p_ = 0.0;
    double lateral_offset_i2p_ = 0.0;
    double longitudinal_offset_i2p_ = 0.0;
    double roll_offset_i2p_ = 0.0;

    double iso_correction_shift_ = 0.0;
    double vertical_correction_shift_ = 0.0;
    double pitch_correction_shift_ = 0.0;
    double lateral_correction_shift_ = 0.0;
    double longitudinal_correction_shift_ = 0.0;
    double roll_correction_shift_ = 0.0;

    std::vector<DeltaAxes> delta_list_;

    wxTextCtrl* entry_iso_abs_ = nullptr;
    wxTextCtrl* entry_pitch_abs_ = nullptr;
    wxTextCtrl* entry_roll_abs_ = nullptr;
    wxTextCtrl* entry_lateral_abs_ = nullptr;
    wxTextCtrl* entry_longitudinal_abs_ = nullptr;
    wxTextCtrl* entry_vertical_abs_ = nullptr;
    wxTextCtrl* entry_column_abs_ = nullptr;

    wxTextCtrl* entry_iso_delta_ = nullptr;
    wxTextCtrl* entry_pitch_delta_ = nullptr;
    wxTextCtrl* entry_roll_delta_ = nullptr;
    wxTextCtrl* entry_lateral_delta_ = nullptr;
    wxTextCtrl* entry_longitudinal_delta_ = nullptr;
    wxTextCtrl* entry_vertical_delta_ = nullptr;
    wxTextCtrl* entry_column_delta_ = nullptr;

    wxTextCtrl* entry_61217_iso_ = nullptr;
    wxTextCtrl* entry_61217_pitch_ = nullptr;
    wxTextCtrl* entry_61217_roll_ = nullptr;
    wxTextCtrl* entry_61217_lateral_ = nullptr;
    wxTextCtrl* entry_61217_longitudinal_ = nullptr;
    wxTextCtrl* entry_61217_vertical_ = nullptr;

    wxTextCtrl* entry_61217_iso_delta_ = nullptr;
    wxTextCtrl* entry_61217_pitch_delta_ = nullptr;
    wxTextCtrl* entry_61217_roll_delta_ = nullptr;
    wxTextCtrl* entry_61217_lateral_delta_ = nullptr;
    wxTextCtrl* entry_61217_longitudinal_delta_ = nullptr;
    wxTextCtrl* entry_61217_vertical_delta_ = nullptr;

    wxTextCtrl* entry_61217_lateral_res_ = nullptr;
    wxTextCtrl* entry_61217_longitudinal_res_ = nullptr;
    wxTextCtrl* entry_61217_vertical_res_ = nullptr;
    wxTextCtrl* entry_61217_pitch_res_ = nullptr;
    wxTextCtrl* entry_61217_roll_res_ = nullptr;
    wxTextCtrl* entry_61217_iso_res_ = nullptr;

    wxTextCtrl* entry_61217_x_res_ = nullptr;
    wxTextCtrl* entry_61217_y_res_ = nullptr;
    wxTextCtrl* entry_61217_z_res_ = nullptr;
    wxTextCtrl* entry_61217_rx_res_ = nullptr;
    wxTextCtrl* entry_61217_ry_res_ = nullptr;
    wxTextCtrl* entry_61217_rz_res_ = nullptr;

    wxTextCtrl* entry_61217_lateral_pps_ = nullptr;
    wxTextCtrl* entry_61217_longitudinal_pps_ = nullptr;
    wxTextCtrl* entry_61217_vertical_pps_ = nullptr;
    wxTextCtrl* entry_61217_pitch_pps_ = nullptr;
    wxTextCtrl* entry_61217_roll_pps_ = nullptr;
    wxTextCtrl* entry_61217_iso_pps_ = nullptr;

    wxButton* btn_iso_plus_ = nullptr;
    wxButton* btn_iso_minus_ = nullptr;
    wxButton* btn_vertical_plus_ = nullptr;
    wxButton* btn_vertical_minus_ = nullptr;
    wxButton* btn_pitch_plus_ = nullptr;
    wxButton* btn_pitch_minus_ = nullptr;
    wxButton* btn_lateral_plus_ = nullptr;
    wxButton* btn_lateral_minus_ = nullptr;
    wxButton* btn_longitudinal_plus_ = nullptr;
    wxButton* btn_longitudinal_minus_ = nullptr;
    wxButton* btn_roll_plus_ = nullptr;
    wxButton* btn_roll_minus_ = nullptr;
    wxButton* btn_column_plus_ = nullptr;
    wxButton* btn_column_minus_ = nullptr;
    wxButton* btn_elev_plus_ = nullptr;
    wxButton* btn_elev_minus_ = nullptr;
    wxButton* btn_azim_plus_ = nullptr;
    wxButton* btn_azim_minus_ = nullptr;
};
