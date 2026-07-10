#pragma once

#include "common.hpp"
#include <vector>
#include <string>
#include <span>

namespace Kaelum {

class Nexus {
public:
    Nexus() = default;
    ~Nexus() = default;

    Nexus(const Nexus&) = delete;
    Nexus& operator=(const Nexus&) = delete;
    Nexus(Nexus&&) = delete;
    Nexus& operator=(Nexus&&) = delete;

    void set_size(uint32_t cols, uint32_t rows);
    void reset();

    void process_bytes(std::span<const char> data);

    const Cell& cell_at(uint32_t row, uint32_t col) const;
    Cell& cell_at(uint32_t row, uint32_t col);
    
    uint32_t rows() const { return grid_size_.rows; }
    uint32_t cols() const { return grid_size_.cols; }
    const GridSize& grid_size() const { return grid_size_; }
    
    void mark_all_dirty();
    void mark_dirty(uint32_t row, uint32_t col);
    bool is_dirty(uint32_t row, uint32_t col) const;
    void clear_dirty(uint32_t row, uint32_t col);

    const CursorPosition& cursor() const { return cursor_; }
    CursorPosition& cursor() { return cursor_; }

private:
    GridSize grid_size_{80, 24};
    std::vector<Cell> grid_;
    std::vector<bool> dirty_;
    CursorPosition cursor_{0, 0};
    
    void ensure_grid_size();
    void scroll_up();
    void handle_char(char32_t cp);
    void handle_control(char c);
    void handle_escape(char c);
    void handle_csi(char c);
    void handle_osc(char c);
    
    enum class State {
        Ground,
        Escape,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        CsiIgnore,
        OscString,
        DcsEntry,
        DcsParam,
        DcsIntermediate,
        DcsPassthrough,
        DcsIgnore,
    } state_ = State::Ground;

    std::vector<int> csi_params_;
    std::string osc_buffer_;
    char32_t last_cp_ = 0;
};

} // namespace Kaelum