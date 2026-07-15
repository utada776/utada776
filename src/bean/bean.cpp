#include "bean.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <vector>

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/timer.h>

namespace {

class BeanGamePanel : public wxPanel {
public:
    explicit BeanGamePanel(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(520, 520), wxBORDER_SIMPLE),
          timer_(this) {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFocus();
        rows_ = static_cast<int>(kMap.size());
        cols_ = static_cast<int>(std::strlen(kMap[0]));
        ResetGame();

        Bind(wxEVT_PAINT, &BeanGamePanel::OnPaint, this);
        Bind(wxEVT_KEY_DOWN, &BeanGamePanel::OnKeyDown, this);
        Bind(wxEVT_TIMER, &BeanGamePanel::OnTimer, this);
        Bind(wxEVT_LEFT_DOWN, &BeanGamePanel::OnClickFocus, this);

        timer_.Start(95);
    }

private:
    struct GhostTrail {
        int r = 0;
        int c = 0;
        int ttl = 0;
    };

    enum class Dir {
        kNone,
        kUp,
        kDown,
        kLeft,
        kRight,
    };

    static constexpr int kEffectTicks = 15000 / 95;
    static constexpr char kRewardClock = 'C';
    static constexpr char kRewardBomb = 'B';
    static constexpr char kRewardStrawberry = 'S';
    static constexpr int kMinSpawnIntervalTicks = 20;
    static constexpr int kMaxSpawnIntervalTicks = 220;
    static constexpr int kMinRewardCap = 1;
    static constexpr int kMaxRewardCap = 10;

    void OnClickFocus(wxMouseEvent& event) {
        (void)event;
        SetFocus();
    }

    void OnKeyDown(wxKeyEvent& event) {
        switch (event.GetKeyCode()) {
            case WXK_UP:
            case 'W':
            case 'w':
                dir_ = Dir::kUp;
                break;
            case WXK_DOWN:
            case 'S':
            case 's':
                dir_ = Dir::kDown;
                break;
            case WXK_LEFT:
            case 'A':
            case 'a':
                dir_ = Dir::kLeft;
                break;
            case WXK_RIGHT:
            case 'D':
            case 'd':
                dir_ = Dir::kRight;
                break;
            case WXK_SPACE:
                if (!win_ && !lose_) {
                    paused_ = !paused_;
                }
                break;
            case '[':
            case '{':
                reward_spawn_interval_ticks_ = std::max(kMinSpawnIntervalTicks, reward_spawn_interval_ticks_ - 10);
                break;
            case ']':
            case '}':
                reward_spawn_interval_ticks_ = std::min(kMaxSpawnIntervalTicks, reward_spawn_interval_ticks_ + 10);
                break;
            case '-':
            case WXK_NUMPAD_SUBTRACT:
                reward_cap_ = std::max(kMinRewardCap, reward_cap_ - 1);
                break;
            case '=':
            case '+':
            case WXK_NUMPAD_ADD:
                reward_cap_ = std::min(kMaxRewardCap, reward_cap_ + 1);
                break;
            case 'R':
            case 'r':
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER:
                ResetGame();
                break;
            default:
                event.Skip();
                return;
        }
        Refresh(false);
    }

    void OnTimer(wxTimerEvent& event) {
        (void)event;
        if (paused_ || win_ || lose_) {
            Refresh(false);
            return;
        }

        ++tick_count_;
        if (freeze_ticks_ > 0) {
            --freeze_ticks_;
        }
        if (invincible_ticks_ > 0) {
            --invincible_ticks_;
        }
        if (explosion_ticks_ > 0) {
            --explosion_ticks_;
        }
        if (tick_count_ % 5 == 0) {
            mouth_open_ = !mouth_open_;
        }

        for (auto& t : ghost_trails_) {
            --t.ttl;
        }
        ghost_trails_.erase(
            std::remove_if(ghost_trails_.begin(), ghost_trails_.end(),
                           [](const GhostTrail& t) { return t.ttl <= 0; }),
            ghost_trails_.end());

        MaybeSpawnReward();

        int dr = 0;
        int dc = 0;
        switch (dir_) {
            case Dir::kUp:
                dr = -1;
                break;
            case Dir::kDown:
                dr = 1;
                break;
            case Dir::kLeft:
                dc = -1;
                break;
            case Dir::kRight:
                dc = 1;
                break;
            default:
                break;
        }

        ++player_tick_;
        if ((dr != 0 || dc != 0) && (player_tick_ % 2 == 0)) {
            const auto next = ResolvePortalMove(player_r_, player_c_, dr, dc);
            if (CanMoveTo(next.first, next.second)) {
                player_r_ = next.first;
                player_c_ = next.second;
                HandlePlayerPickup();
            }
        }

        if (!win_ && !lose_ && freeze_ticks_ <= 0) {
            MoveGhost();
        }

        ResolveCollision();

        if (beans_left_ <= 0) {
            win_ = true;
            timer_.Stop();
        }

        Refresh(false);
    }

    void ResetGame() {
        map_.assign(kMap.begin(), kMap.end());
        AddBorderPortals();
        EnsureMazeConnected();
        RemoveMazeDeadEnds();

        beans_left_ = 0;
        for (int r = 0; r < rows_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                if (map_[r][c] == '.') {
                    ++beans_left_;
                }
            }
        }

        player_r_ = 10;
        player_c_ = 10;
        ghost_r_ = 1;
        ghost_c_ = cols_ - 2;
        dir_ = Dir::kNone;
        paused_ = false;
        win_ = false;
        lose_ = false;
        score_ = 0;
        ghost_tick_ = 0;
        player_tick_ = 0;
        freeze_ticks_ = 0;
        invincible_ticks_ = 0;
        explosion_ticks_ = 0;
        tick_count_ = 0;
        mouth_open_ = true;
        explosion_center_r_ = -1;
        explosion_center_c_ = -1;
        ghost_trails_.clear();
        timer_.Start(95);
        Refresh(false);
    }

    int MidRow() const {
        return rows_ / 2;
    }

    int MidCol() const {
        return cols_ / 2;
    }

    bool IsPortalCell(int r, int c) const {
        return (r == 0 && c == MidCol()) || (r == rows_ - 1 && c == MidCol()) ||
               (r == MidRow() && c == 0) || (r == MidRow() && c == cols_ - 1);
    }

    bool IsTeleportStep(int from_r, int from_c, int to_r, int to_c) const {
        if (!IsPortalCell(from_r, from_c) && !IsPortalCell(to_r, to_c)) {
            return false;
        }
        return std::abs(from_r - to_r) > 1 || std::abs(from_c - to_c) > 1;
    }

    void PushGhostTrail(int r, int c) {
        ghost_trails_.push_back({r, c, 12});
        if (ghost_trails_.size() > 28) {
            ghost_trails_.erase(ghost_trails_.begin());
        }
    }

    void AddBorderPortals() {
        const int mid_r = MidRow();
        const int mid_c = MidCol();

        map_[0][mid_c] = ' ';
        map_[rows_ - 1][mid_c] = ' ';
        map_[mid_r][0] = ' ';
        map_[mid_r][cols_ - 1] = ' ';

        EnsureWalkable(1, mid_c);
        EnsureWalkable(rows_ - 2, mid_c);
        EnsureWalkable(mid_r, 1);
        EnsureWalkable(mid_r, cols_ - 2);
    }

    std::pair<int, int> ResolvePortalMove(int r, int c, int dr, int dc) const {
        int nr = r + dr;
        int nc = c + dc;
        const int mid_r = MidRow();
        const int mid_c = MidCol();

        if (nr < 0 && r == 0 && c == mid_c) {
            nr = rows_ - 1;
            nc = mid_c;
        } else if (nr >= rows_ && r == rows_ - 1 && c == mid_c) {
            nr = 0;
            nc = mid_c;
        } else if (nc < 0 && c == 0 && r == mid_r) {
            nr = mid_r;
            nc = cols_ - 1;
        } else if (nc >= cols_ && c == cols_ - 1 && r == mid_r) {
            nr = mid_r;
            nc = 0;
        }

        return {nr, nc};
    }

    bool IsWalkableCell(int r, int c) const {
        if (r < 0 || c < 0 || r >= rows_ || c >= cols_) {
            return false;
        }
        return map_[r][c] != '#';
    }

    int CountWalkableNeighbors(int r, int c) const {
        int n = 0;
        if (IsWalkableCell(r - 1, c)) {
            ++n;
        }
        if (IsWalkableCell(r + 1, c)) {
            ++n;
        }
        if (IsWalkableCell(r, c - 1)) {
            ++n;
        }
        if (IsWalkableCell(r, c + 1)) {
            ++n;
        }
        return n;
    }

    void EnsureWalkable(int r, int c, bool place_bean = true) {
        if (r <= 0 || c <= 0 || r >= rows_ - 1 || c >= cols_ - 1) {
            return;
        }
        if (map_[r][c] == '#') {
            map_[r][c] = place_bean ? '.' : ' ';
        }
    }

    void EnsureMazeConnected() {
        std::vector<std::vector<int>> visited(rows_, std::vector<int>(cols_, 0));
        std::deque<std::pair<int, int>> q;

        for (int r = 1; r < rows_ - 1; ++r) {
            for (int c = 1; c < cols_ - 1; ++c) {
                if (IsWalkableCell(r, c)) {
                    q.push_back({r, c});
                    visited[r][c] = 1;
                    r = rows_;
                    break;
                }
            }
        }

        while (!q.empty()) {
            const auto cur = q.front();
            q.pop_front();
            const int r = cur.first;
            const int c = cur.second;
            const std::array<std::pair<int, int>, 4> ds = {
                std::make_pair(-1, 0), std::make_pair(1, 0), std::make_pair(0, -1), std::make_pair(0, 1)};
            for (const auto& d : ds) {
                const int nr = r + d.first;
                const int nc = c + d.second;
                if (nr <= 0 || nc <= 0 || nr >= rows_ - 1 || nc >= cols_ - 1) {
                    continue;
                }
                if (!visited[nr][nc] && IsWalkableCell(nr, nc)) {
                    visited[nr][nc] = 1;
                    q.push_back({nr, nc});
                }
            }
        }

        auto nearest_visited = [&](int r, int c) {
            int best_dist = rows_ + cols_ + 100;
            std::pair<int, int> best = {r, c};
            for (int rr = 1; rr < rows_ - 1; ++rr) {
                for (int cc = 1; cc < cols_ - 1; ++cc) {
                    if (!visited[rr][cc]) {
                        continue;
                    }
                    const int d = std::abs(rr - r) + std::abs(cc - c);
                    if (d < best_dist) {
                        best_dist = d;
                        best = {rr, cc};
                    }
                }
            }
            return best;
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (int r = 1; r < rows_ - 1; ++r) {
                for (int c = 1; c < cols_ - 1; ++c) {
                    if (!IsWalkableCell(r, c) || visited[r][c]) {
                        continue;
                    }

                    const auto anchor = nearest_visited(r, c);
                    int cr = r;
                    int cc = c;
                    while (cr != anchor.first) {
                        cr += (anchor.first > cr) ? 1 : -1;
                        EnsureWalkable(cr, cc);
                    }
                    while (cc != anchor.second) {
                        cc += (anchor.second > cc) ? 1 : -1;
                        EnsureWalkable(cr, cc);
                    }
                    changed = true;
                }
            }

            if (changed) {
                for (auto& row : visited) {
                    std::fill(row.begin(), row.end(), 0);
                }
                std::deque<std::pair<int, int>> qq;
                qq.push_back({1, 1});
                if (!IsWalkableCell(1, 1)) {
                    for (int r = 1; r < rows_ - 1 && qq.empty(); ++r) {
                        for (int c = 1; c < cols_ - 1; ++c) {
                            if (IsWalkableCell(r, c)) {
                                qq.push_back({r, c});
                                break;
                            }
                        }
                    }
                }
                if (!qq.empty()) {
                    visited[qq.front().first][qq.front().second] = 1;
                }
                while (!qq.empty()) {
                    const auto cur = qq.front();
                    qq.pop_front();
                    const int r = cur.first;
                    const int c = cur.second;
                    const std::array<std::pair<int, int>, 4> ds = {
                        std::make_pair(-1, 0), std::make_pair(1, 0), std::make_pair(0, -1), std::make_pair(0, 1)};
                    for (const auto& d : ds) {
                        const int nr = r + d.first;
                        const int nc = c + d.second;
                        if (nr <= 0 || nc <= 0 || nr >= rows_ - 1 || nc >= cols_ - 1) {
                            continue;
                        }
                        if (!visited[nr][nc] && IsWalkableCell(nr, nc)) {
                            visited[nr][nc] = 1;
                            qq.push_back({nr, nc});
                        }
                    }
                }
            }
        }
    }

    void RemoveMazeDeadEnds() {
        bool changed = true;
        while (changed) {
            changed = false;
            for (int r = 1; r < rows_ - 1; ++r) {
                for (int c = 1; c < cols_ - 1; ++c) {
                    if (!IsWalkableCell(r, c)) {
                        continue;
                    }

                    const bool is_spawn = (r == player_r_ && c == player_c_) || (r == 1 && c == cols_ - 2);
                    if (is_spawn) {
                        continue;
                    }

                    if (CountWalkableNeighbors(r, c) > 1) {
                        continue;
                    }

                    std::vector<std::pair<int, int>> open_candidates;
                    const std::array<std::pair<int, int>, 4> ds = {
                        std::make_pair(-1, 0), std::make_pair(1, 0), std::make_pair(0, -1), std::make_pair(0, 1)};

                    for (const auto& d : ds) {
                        const int nr = r + d.first;
                        const int nc = c + d.second;
                        if (nr <= 0 || nc <= 0 || nr >= rows_ - 1 || nc >= cols_ - 1) {
                            continue;
                        }
                        if (map_[nr][nc] != '#') {
                            continue;
                        }

                        const int br = nr + d.first;
                        const int bc = nc + d.second;
                        if (br <= 0 || bc <= 0 || br >= rows_ - 1 || bc >= cols_ - 1) {
                            continue;
                        }
                        if (IsWalkableCell(br, bc)) {
                            open_candidates.push_back({nr, nc});
                        }
                    }

                    if (open_candidates.empty()) {
                        for (const auto& d : ds) {
                            const int nr = r + d.first;
                            const int nc = c + d.second;
                            if (nr <= 0 || nc <= 0 || nr >= rows_ - 1 || nc >= cols_ - 1) {
                                continue;
                            }
                            if (map_[nr][nc] == '#') {
                                open_candidates.push_back({nr, nc});
                            }
                        }
                    }

                    if (!open_candidates.empty()) {
                        std::uniform_int_distribution<int> pick(0, static_cast<int>(open_candidates.size()) - 1);
                        const auto cell = open_candidates[pick(rng_)];
                        map_[cell.first][cell.second] = '.';
                        changed = true;
                    }
                }
            }
        }
    }

    void HandlePlayerPickup() {
        const char v = map_[player_r_][player_c_];
        if (v == '.') {
            map_[player_r_][player_c_] = ' ';
            ++score_;
            --beans_left_;
            high_score_ = std::max(high_score_, score_);
            return;
        }
        if (v == kRewardClock) {
            map_[player_r_][player_c_] = ' ';
            freeze_ticks_ = kEffectTicks;
            score_ += 3;
            high_score_ = std::max(high_score_, score_);
            return;
        }
        if (v == kRewardBomb) {
            map_[player_r_][player_c_] = ' ';
            TriggerBomb(player_r_, player_c_);
            score_ += 2;
            high_score_ = std::max(high_score_, score_);
            return;
        }
        if (v == kRewardStrawberry) {
            map_[player_r_][player_c_] = ' ';
            invincible_ticks_ = kEffectTicks;
            score_ += 4;
            high_score_ = std::max(high_score_, score_);
            return;
        }
    }

    void ResolveCollision() {
        if (ghost_r_ != player_r_ || ghost_c_ != player_c_) {
            return;
        }
        if (invincible_ticks_ > 0) {
            score_ += 8;
            high_score_ = std::max(high_score_, score_);
            RespawnGhost();
            return;
        }

        lose_ = true;
        timer_.Stop();
    }

    void RespawnGhost() {
        ghost_r_ = 1;
        ghost_c_ = cols_ - 2;
    }

    void TriggerBomb(int center_r, int center_c) {
        explosion_center_r_ = center_r;
        explosion_center_c_ = center_c;
        explosion_ticks_ = 10;

        for (int r = center_r - 1; r <= center_r + 2; ++r) {
            for (int c = center_c - 1; c <= center_c + 2; ++c) {
                if (r < 0 || c < 0 || r >= rows_ || c >= cols_) {
                    continue;
                }
                if (map_[r][c] == '#') {
                    continue;
                }
                if (map_[r][c] == '.') {
                    --beans_left_;
                    ++score_;
                }
                map_[r][c] = ' ';
            }
        }
        high_score_ = std::max(high_score_, score_);
    }

    void MaybeSpawnReward() {
        if (tick_count_ % reward_spawn_interval_ticks_ != 0) {
            return;
        }

        if (CountActiveRewards() >= reward_cap_) {
            return;
        }

        std::vector<std::pair<int, int>> free_cells;
        for (int r = 1; r < rows_ - 1; ++r) {
            for (int c = 1; c < cols_ - 1; ++c) {
                if (map_[r][c] == ' ') {
                    free_cells.push_back({r, c});
                }
            }
        }
        if (free_cells.empty()) {
            return;
        }

        std::uniform_int_distribution<int> pick_cell(0, static_cast<int>(free_cells.size()) - 1);
        const auto cell = free_cells[pick_cell(rng_)];

        std::uniform_int_distribution<int> pick_reward(0, 2);
        const int reward = pick_reward(rng_);
        if (reward == 0) {
            map_[cell.first][cell.second] = kRewardClock;
        } else if (reward == 1) {
            map_[cell.first][cell.second] = kRewardBomb;
        } else {
            map_[cell.first][cell.second] = kRewardStrawberry;
        }
    }

    int CountActiveRewards() const {
        int cnt = 0;
        for (int r = 0; r < rows_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                const char v = map_[r][c];
                if (v == kRewardClock || v == kRewardBomb || v == kRewardStrawberry) {
                    ++cnt;
                }
            }
        }
        return cnt;
    }

    void MoveGhost() {
        ++ghost_tick_;
        if (ghost_tick_ % 8 != 0) {
            return;
        }

        const std::array<std::pair<int, int>, 4> dirs = {
            std::make_pair(-1, 0), std::make_pair(1, 0), std::make_pair(0, -1), std::make_pair(0, 1)};

        std::vector<std::pair<int, int>> valid;
        for (const auto& d : dirs) {
            const auto next = ResolvePortalMove(ghost_r_, ghost_c_, d.first, d.second);
            if (CanMoveTo(next.first, next.second)) {
                valid.push_back(next);
            }
        }
        if (valid.empty()) {
            return;
        }

        std::uniform_int_distribution<int> coin(0, 99);
        if (invincible_ticks_ > 0) {
            std::sort(valid.begin(), valid.end(), [&](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                const int da = std::abs(a.first - player_r_) + std::abs(a.second - player_c_);
                const int db = std::abs(b.first - player_r_) + std::abs(b.second - player_c_);
                return da > db;
            });
            if (coin(rng_) < 30) {
                std::uniform_int_distribution<int> pick(0, static_cast<int>(valid.size()) - 1);
                const auto selected = valid[pick(rng_)];
                ghost_r_ = selected.first;
                ghost_c_ = selected.second;
                return;
            }
            ghost_r_ = valid.front().first;
            ghost_c_ = valid.front().second;
            return;
        }

        if (coin(rng_) < 25) {
            std::uniform_int_distribution<int> pick(0, static_cast<int>(valid.size()) - 1);
            const auto selected = valid[pick(rng_)];
            ghost_r_ = selected.first;
            ghost_c_ = selected.second;
            return;
        }

        int best_dist = rows_ * cols_;
        std::pair<int, int> best = valid.front();
        for (const auto& v : valid) {
            const int d = ShortestDistance(v.first, v.second, player_r_, player_c_);
            if (d < best_dist) {
                best_dist = d;
                best = v;
            }
        }

        const int old_r = ghost_r_;
        const int old_c = ghost_c_;
        ghost_r_ = best.first;
        ghost_c_ = best.second;
        if (IsTeleportStep(old_r, old_c, ghost_r_, ghost_c_)) {
            PushGhostTrail(old_r, old_c);
            PushGhostTrail(ghost_r_, ghost_c_);
        }
    }

    int ShortestDistance(int sr, int sc, int tr, int tc) const {
        if (sr == tr && sc == tc) {
            return 0;
        }

        std::vector<std::vector<int>> dist(rows_, std::vector<int>(cols_, -1));
        std::deque<std::pair<int, int>> q;
        q.push_back({sr, sc});
        dist[sr][sc] = 0;

        const std::array<std::pair<int, int>, 4> dirs = {
            std::make_pair(-1, 0), std::make_pair(1, 0), std::make_pair(0, -1), std::make_pair(0, 1)};

        while (!q.empty()) {
            const auto cur = q.front();
            q.pop_front();
            const int r = cur.first;
            const int c = cur.second;

            for (const auto& d : dirs) {
                const auto next = ResolvePortalMove(r, c, d.first, d.second);
                const int nr = next.first;
                const int nc = next.second;
                if (!CanMoveTo(nr, nc) || dist[nr][nc] >= 0) {
                    continue;
                }
                dist[nr][nc] = dist[r][c] + 1;
                if (nr == tr && nc == tc) {
                    return dist[nr][nc];
                }
                q.push_back({nr, nc});
            }
        }

        return rows_ * cols_;
    }

    bool CanMoveTo(int r, int c) const {
        if (r < 0 || c < 0 || r >= rows_ || c >= cols_) {
            return false;
        }
        return map_[r][c] != '#';
    }

    void OnPaint(wxPaintEvent& event) {
        (void)event;
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxColour(11, 13, 20)));
        dc.Clear();
        wxGCDC gdc(dc);
        gdc.SetBackground(wxBrush(wxColour(11, 13, 20)));

        const wxSize sz = GetClientSize();
        const int tile = std::max(14, std::min((sz.GetWidth() - 40) / cols_, (sz.GetHeight() - 124) / rows_));
        const int board_w = cols_ * tile;
        const int board_h = rows_ * tile;
        const int ox = (sz.GetWidth() - board_w) / 2;
        const int oy = 58;

        gdc.SetPen(*wxTRANSPARENT_PEN);
        gdc.SetBrush(wxBrush(wxColour(22, 26, 38)));
        gdc.DrawRoundedRectangle(ox - 18, oy - 18, board_w + 36, board_h + 36, 16);
        gdc.SetBrush(wxBrush(wxColour(14, 17, 26)));
        gdc.DrawRoundedRectangle(ox - 10, oy - 10, board_w + 20, board_h + 20, 12);

        gdc.SetPen(*wxTRANSPARENT_PEN);
        for (int r = 0; r < rows_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                const wxRect cell(ox + c * tile, oy + r * tile, tile, tile);
                const char v = map_[r][c];
                if (v == '#') {
                    gdc.SetBrush(wxBrush(wxColour(40, 112, 246)));
                    gdc.DrawRoundedRectangle(cell, 4);
                    gdc.SetPen(wxPen(wxColour(96, 171, 255), 1));
                    gdc.DrawLine(cell.GetX() + 1, cell.GetY() + 1, cell.GetRight() - 1, cell.GetY() + 1);
                    gdc.DrawLine(cell.GetX() + 1, cell.GetY() + 1, cell.GetX() + 1, cell.GetBottom() - 1);
                    gdc.SetPen(*wxTRANSPARENT_PEN);
                } else {
                    gdc.SetBrush(wxBrush(wxColour(18, 21, 33)));
                    gdc.DrawRectangle(cell);
                    gdc.SetPen(wxPen(wxColour(28, 34, 48), 1));
                    gdc.DrawLine(cell.GetX(), cell.GetBottom(), cell.GetRight(), cell.GetBottom());
                    gdc.SetPen(*wxTRANSPARENT_PEN);
                    if (v == '.') {
                        gdc.SetBrush(wxBrush(wxColour(236, 244, 255)));
                        const int cr = std::max(2, tile / 7);
                        gdc.DrawCircle(cell.GetX() + tile / 2, cell.GetY() + tile / 2, cr);
                    } else if (v == kRewardClock) {
                        gdc.SetBrush(wxBrush(wxColour(70, 220, 125)));
                        gdc.DrawCircle(cell.GetX() + tile / 2, cell.GetY() + tile / 2, std::max(4, tile / 3));
                        gdc.SetPen(wxPen(wxColour(12, 60, 20), 2));
                        gdc.DrawLine(cell.GetX() + tile / 2, cell.GetY() + tile / 2,
                                    cell.GetX() + tile / 2, cell.GetY() + tile / 2 - std::max(3, tile / 6));
                        gdc.DrawLine(cell.GetX() + tile / 2, cell.GetY() + tile / 2,
                                    cell.GetX() + tile / 2 + std::max(3, tile / 7), cell.GetY() + tile / 2);
                        gdc.SetPen(*wxTRANSPARENT_PEN);
                    } else if (v == kRewardBomb) {
                        gdc.SetBrush(wxBrush(wxColour(176, 92, 242)));
                        gdc.DrawCircle(cell.GetX() + tile / 2, cell.GetY() + tile / 2, std::max(4, tile / 3));
                        gdc.SetBrush(wxBrush(wxColour(245, 225, 120)));
                        gdc.DrawRectangle(cell.GetX() + tile / 2 - 1, cell.GetY() + tile / 2 - tile / 3, 3, tile / 4);
                    } else if (v == kRewardStrawberry) {
                        gdc.SetBrush(wxBrush(wxColour(255, 112, 186)));
                        gdc.DrawCircle(cell.GetX() + tile / 2 - std::max(2, tile / 8), cell.GetY() + tile / 2,
                                      std::max(3, tile / 4));
                        gdc.DrawCircle(cell.GetX() + tile / 2 + std::max(2, tile / 8), cell.GetY() + tile / 2,
                                      std::max(3, tile / 4));
                        gdc.SetBrush(wxBrush(wxColour(120, 220, 120)));
                        gdc.DrawRectangle(cell.GetX() + tile / 2 - 2, cell.GetY() + tile / 2 - tile / 2, 4, tile / 4);
                    }
                }
            }
        }

        const int portal_pulse = tick_count_ % 24;
        const int glow = 130 + (portal_pulse < 12 ? portal_pulse * 8 : (24 - portal_pulse) * 8);
        const int flow_phase = tick_count_ % 18;

        auto draw_portal = [&](int r, int c, const wxColour& base_color, int dir_r, int dir_c) {
            const wxColour portal_outer(base_color.Red(), base_color.Green(), base_color.Blue(), std::min(245, glow));
            const wxColour portal_inner(220, 245, 255, std::min(255, glow + 8));
            const wxPen portal_pen_outer(portal_outer, 3);
            const wxPen portal_pen_inner(portal_inner, 1);
            const wxRect cell(ox + c * tile, oy + r * tile, tile, tile);
            gdc.SetBrush(*wxTRANSPARENT_BRUSH);
            gdc.SetPen(portal_pen_outer);
            gdc.DrawRoundedRectangle(cell.GetX() + 1, cell.GetY() + 1, tile - 2, tile - 2, 4);
            gdc.SetPen(portal_pen_inner);
            gdc.DrawRoundedRectangle(cell.GetX() + 3, cell.GetY() + 3, tile - 6, tile - 6, 3);

            // Flow streaks indicate the teleport direction for this portal.
            gdc.SetPen(wxPen(wxColour(240, 252, 255, std::min(255, glow + 6)), 2));
            for (int i = 0; i < 3; ++i) {
                const int p = (flow_phase + i * 6) % 18;
                const int cx = cell.GetX() + tile / 2 + dir_c * (p - 9) * std::max(1, tile / 18);
                const int cy = cell.GetY() + tile / 2 + dir_r * (p - 9) * std::max(1, tile / 18);
                const int lx = std::max(2, tile / 8);
                if (dir_r != 0) {
                    gdc.DrawLine(cx - lx, cy, cx + lx, cy);
                } else {
                    gdc.DrawLine(cx, cy - lx, cx, cy + lx);
                }
            }
            gdc.SetPen(*wxTRANSPARENT_PEN);
        };

        draw_portal(0, MidCol(), wxColour(45, 200, 255), 1, 0);
        draw_portal(rows_ - 1, MidCol(), wxColour(52, 255, 180), -1, 0);
        draw_portal(MidRow(), 0, wxColour(255, 170, 70), 0, 1);
        draw_portal(MidRow(), cols_ - 1, wxColour(245, 105, 220), 0, -1);

        for (const auto& trail : ghost_trails_) {
            const wxRect tcell(ox + trail.c * tile, oy + trail.r * tile, tile, tile);
            const int alpha = std::max(20, trail.ttl * 16);
            gdc.SetBrush(wxBrush(wxColour(110, 220, 255, alpha)));
            gdc.SetPen(*wxTRANSPARENT_PEN);
            gdc.DrawCircle(tcell.GetX() + tile / 2, tcell.GetY() + tile / 2, std::max(4, tile / 3));
            gdc.SetBrush(wxBrush(wxColour(190, 246, 255, std::min(255, alpha + 20))));
            gdc.DrawCircle(tcell.GetX() + tile / 2, tcell.GetY() + tile / 2, std::max(2, tile / 5));
        }

        const wxRect pcell(ox + player_c_ * tile, oy + player_r_ * tile, tile, tile);
        gdc.SetBrush(wxBrush(wxColour(0, 0, 0, 70)));
        gdc.SetPen(*wxTRANSPARENT_PEN);
        gdc.DrawEllipse(pcell.GetX() + tile / 5, pcell.GetBottom() - tile / 5, tile * 3 / 5, tile / 4);
        gdc.SetBrush(wxBrush(wxColour(255, 212, 0)));
        gdc.DrawCircle(pcell.GetX() + tile / 2, pcell.GetY() + tile / 2, tile / 2 - 2);
        gdc.SetBrush(wxBrush(wxColour(255, 245, 168)));
        gdc.DrawCircle(pcell.GetX() + tile / 2 - tile / 6, pcell.GetY() + tile / 3, std::max(2, tile / 7));

        // Smile face with mouth animation.
        gdc.SetBrush(wxBrush(wxColour(20, 20, 20)));
        gdc.DrawCircle(pcell.GetX() + tile / 2 - tile / 6, pcell.GetY() + tile / 2 - tile / 7, std::max(1, tile / 12));
        gdc.DrawCircle(pcell.GetX() + tile / 2 + tile / 6, pcell.GetY() + tile / 2 - tile / 7, std::max(1, tile / 12));
        if (mouth_open_) {
            wxPoint p1(pcell.GetX() + tile / 2, pcell.GetY() + tile / 2);
            wxPoint p2 = p1;
            wxPoint p3 = p1;
            const int mouth_len = std::max(6, tile / 2);
            const int mouth_w = std::max(4, tile / 5);
            switch (dir_) {
                case Dir::kUp:
                    p2.x -= mouth_w;
                    p2.y -= mouth_len;
                    p3.x += mouth_w;
                    p3.y -= mouth_len;
                    break;
                case Dir::kDown:
                    p2.x -= mouth_w;
                    p2.y += mouth_len;
                    p3.x += mouth_w;
                    p3.y += mouth_len;
                    break;
                case Dir::kLeft:
                    p2.x -= mouth_len;
                    p2.y -= mouth_w;
                    p3.x -= mouth_len;
                    p3.y += mouth_w;
                    break;
                default:
                    p2.x += mouth_len;
                    p2.y -= mouth_w;
                    p3.x += mouth_len;
                    p3.y += mouth_w;
                    break;
            }
            wxPoint tri[3] = {p1, p2, p3};
            gdc.SetBrush(wxBrush(wxColour(8, 8, 12)));
            gdc.DrawPolygon(3, tri);
        } else {
            gdc.SetPen(wxPen(wxColour(20, 20, 20), 2));
            gdc.SetBrush(*wxTRANSPARENT_BRUSH);
            gdc.DrawEllipticArc(pcell.GetX() + tile / 4, pcell.GetY() + tile / 2 - 1, tile / 2, tile / 3, 200, 340);
            gdc.SetPen(*wxTRANSPARENT_PEN);
        }

        const wxRect gcell(ox + ghost_c_ * tile, oy + ghost_r_ * tile, tile, tile);
        const bool fear_mode = invincible_ticks_ > 0;
        const bool fear_flash = fear_mode && ((tick_count_ / 2) % 2 == 0);
        const wxColour ghost_color = !fear_mode
                                         ? wxColour(255, 84, 84)
                                         : (fear_flash ? wxColour(108, 200, 255) : wxColour(176, 235, 255));
        gdc.SetBrush(wxBrush(wxColour(0, 0, 0, 70)));
        gdc.SetPen(*wxTRANSPARENT_PEN);
        gdc.DrawEllipse(gcell.GetX() + tile / 5, gcell.GetBottom() - tile / 5, tile * 3 / 5, tile / 4);
        gdc.SetBrush(wxBrush(ghost_color));
        gdc.DrawRoundedRectangle(gcell.GetX(), gcell.GetY() + tile / 6, tile, tile * 5 / 6, 5);
        gdc.DrawCircle(gcell.GetX() + tile / 3, gcell.GetY() + tile / 3, tile / 4);
        gdc.DrawCircle(gcell.GetX() + tile * 2 / 3, gcell.GetY() + tile / 3, tile / 4);
        gdc.SetBrush(wxBrush(wxColour(250, 250, 250)));
        gdc.DrawCircle(gcell.GetX() + tile / 3, gcell.GetY() + tile / 3, std::max(2, tile / 8));
        gdc.DrawCircle(gcell.GetX() + tile * 2 / 3, gcell.GetY() + tile / 3, std::max(2, tile / 8));
        gdc.SetBrush(wxBrush(wxColour(18, 18, 22)));
        if (fear_mode) {
            gdc.DrawCircle(gcell.GetX() + tile / 3 + (fear_flash ? -1 : 1), gcell.GetY() + tile / 3,
                          std::max(1, tile / 16));
            gdc.DrawCircle(gcell.GetX() + tile * 2 / 3 + (fear_flash ? -1 : 1), gcell.GetY() + tile / 3,
                          std::max(1, tile / 16));
        } else {
            gdc.DrawCircle(gcell.GetX() + tile / 3, gcell.GetY() + tile / 3, std::max(1, tile / 14));
            gdc.DrawCircle(gcell.GetX() + tile * 2 / 3, gcell.GetY() + tile / 3, std::max(1, tile / 14));
        }
        gdc.SetPen(wxPen(wxColour(20, 20, 20), 2));
        if (fear_mode) {
            gdc.DrawLine(gcell.GetX() + tile / 3, gcell.GetY() + tile * 3 / 4,
                        gcell.GetX() + tile / 2, gcell.GetY() + tile * 2 / 3);
            gdc.DrawLine(gcell.GetX() + tile / 2, gcell.GetY() + tile * 2 / 3,
                        gcell.GetX() + tile * 2 / 3, gcell.GetY() + tile * 3 / 4);
            gdc.SetBrush(wxBrush(wxColour(180, 240, 255)));
            gdc.SetPen(*wxTRANSPARENT_PEN);
            gdc.DrawEllipse(gcell.GetX() + tile - std::max(5, tile / 5), gcell.GetY() + tile / 2,
                           std::max(4, tile / 7), std::max(6, tile / 4));
            gdc.SetPen(wxPen(wxColour(20, 20, 20), 2));
        } else {
            gdc.DrawLine(gcell.GetX() + tile / 3, gcell.GetY() + tile * 2 / 3,
                        gcell.GetX() + tile / 2, gcell.GetY() + tile * 3 / 4);
            gdc.DrawLine(gcell.GetX() + tile / 2, gcell.GetY() + tile * 3 / 4,
                        gcell.GetX() + tile * 2 / 3, gcell.GetY() + tile * 2 / 3);
        }
        gdc.SetPen(*wxTRANSPARENT_PEN);

        if (explosion_ticks_ > 0 && explosion_center_r_ >= 0 && explosion_center_c_ >= 0) {
            const int cx = ox + explosion_center_c_ * tile + tile / 2;
            const int cy = oy + explosion_center_r_ * tile + tile / 2;
            const int r1 = std::max(6, (11 - explosion_ticks_) * tile / 6);
            const int r2 = std::max(10, (12 - explosion_ticks_) * tile / 4);
            gdc.SetBrush(*wxTRANSPARENT_BRUSH);
            gdc.SetPen(wxPen(wxColour(255, 120, 220), 2));
            gdc.DrawCircle(cx, cy, r2);
            gdc.SetPen(wxPen(wxColour(255, 210, 90), 3));
            gdc.DrawCircle(cx, cy, r1);
            gdc.SetPen(*wxTRANSPARENT_PEN);
        }

        gdc.SetTextForeground(wxColour(236, 240, 248));
        gdc.DrawText(wxString::Format(wxT("Score: %d  Beans Left: %d"), score_, std::max(beans_left_, 0)), 14, 14);
        gdc.DrawText(wxString::Format(wxT("High Score: %d"), high_score_), sz.GetWidth() - 170, 14);

        const int info_y = oy + board_h + 8;
        gdc.DrawText(wxT("Control: Arrow Keys / WASD move, Space pause, R/Enter restart"), 12, info_y);
        gdc.DrawText(wxT("Portals: top/bottom/left/right exits wrap to opposite side"), 12, info_y + 20);
        gdc.DrawText(wxT("Tune Spawn: [ / ] interval, - / + reward cap"), 12, info_y + 40);
        gdc.DrawText(wxT("Rewards: Green Clock freeze ghost 15s | Purple Bomb clear 4x4 | Pink Strawberry invincible 15s"),
                    12, info_y + 60);
        gdc.DrawText(wxString::Format(wxT("Spawn Interval: %d ticks | Reward Cap: %d | Active Rewards: %d"),
                         reward_spawn_interval_ticks_, reward_cap_, CountActiveRewards()),
                    12, info_y + 80);

        if (paused_ && !win_ && !lose_) {
            gdc.SetTextForeground(wxColour(255, 230, 120));
            gdc.DrawText(wxT("Paused"), sz.GetWidth() - 90, 12);
        }
        if (win_) {
            gdc.SetTextForeground(wxColour(120, 255, 160));
            gdc.DrawText(wxT("You cleared all beans! Press R to replay."), sz.GetWidth() / 2 - 140, 12);
        }
        if (lose_) {
            gdc.SetTextForeground(wxColour(255, 140, 120));
            gdc.DrawText(wxT("Caught by ghost! Press R to restart."), sz.GetWidth() / 2 - 130, 12);
        }
    }

    static constexpr std::array<const char*, 21> kMap = {
        "#####################",
        "#.........#.........#",
        "#.###.###.#.###.###.#",
        "#.# #.# #.#.# #.# #.#",
        "#...................#",
        "#.###.#.#####.#.###.#",
        "#.....#...#...#.....#",
        "#####.### # ###.#####",
        "#   #.#       #.#   #",
        "#####.# ## ## #.#####",
        "#.......#   #.......#",
        "#####.# ##### #.#####",
        "#   #.#       #.#   #",
        "#####.#.#####.#.#####",
        "#.........#.........#",
        "#.###.###.#.###.###.#",
        "#...#..... .....#...#",
        "###.#.#.#####.#.#.###",
        "#.....#...#...#.....#",
        "#.######### #######.#",
        "#####################",
    };

    wxTimer timer_;
    int rows_ = 0;
    int cols_ = 0;
    std::vector<std::string> map_;

    int player_r_ = 10;
    int player_c_ = 10;
    int ghost_r_ = 1;
    int ghost_c_ = 1;
    Dir dir_ = Dir::kNone;
    bool paused_ = false;
    bool win_ = false;
    bool lose_ = false;

    int score_ = 0;
    int high_score_ = 0;
    int beans_left_ = 0;
    int ghost_tick_ = 0;
    int player_tick_ = 0;
    int freeze_ticks_ = 0;
    int invincible_ticks_ = 0;
    int explosion_ticks_ = 0;
    int explosion_center_r_ = -1;
    int explosion_center_c_ = -1;
    int tick_count_ = 0;
    bool mouth_open_ = true;
    int reward_spawn_interval_ticks_ = 80;
    int reward_cap_ = 4;
    std::vector<GhostTrail> ghost_trails_;
    std::mt19937 rng_{std::random_device{}()};
};

}  // namespace

namespace bean_style {

wxString GameTitle() {
    return wxT("Bean Game");
}

wxSize WindowSize() {
    return wxSize(760, 820);
}

wxSize MinWindowSize() {
    return wxSize(620, 700);
}

wxSize GamePanelSize() {
    return wxSize(520, 520);
}

int TimerIntervalMs() {
    return 95;
}

}  // namespace bean_style

BeanFrame::BeanFrame(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, bean_style::GameTitle(), wxDefaultPosition, bean_style::WindowSize()) {
    SetMinSize(bean_style::MinWindowSize());
    wxPanel* root = new wxPanel(this);
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* title = new wxStaticText(root, wxID_ANY, wxT("Keyboard Bean Game"));
    wxFont title_font(12, wxFONTFAMILY_SWISS, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD);
    title->SetFont(title_font);
    sizer->Add(title, 0, wxALL | wxALIGN_CENTER_HORIZONTAL, 10);

    BeanGamePanel* game_panel = new BeanGamePanel(root);
    sizer->Add(game_panel, 1, wxALL | wxEXPAND, 10);

    root->SetSizer(sizer);
    Centre();
}
