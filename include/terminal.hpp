#pragma once

#include "common.hpp"
#include <vector>
#include <span>
#include <cstddef>

namespace Kaelum {

class Terminal {
public:
    enum class State {
        Ground,
        Escape,
        CsiEntry,
        CsiParam,
        CsiIntermediate,
        OscString,
    };

    Terminal() = default;
    ~Terminal() = default;

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    Terminal(Terminal&&) = delete;
    Terminal& operator=(Terminal&&) = delete;

    void set_size(uint32_t cols, uint32_t rows);
    void reset();
    void process_bytes(std::span<const char> data);
    void process_char(char c);

    const std::vector<Cell>& grid() const { return grid_; }
    const std::vector<bool>& dirty() const { return dirty_; }
    const CursorPosition& cursor() const { return cursor_; }
    const GridSize& grid_size() const { return grid_size_; }

private:
    void handle_control(char c);
    void handle_escape(char c);
    void handle_csi(char c);
    void handle_osc(char c);
    void handle_char(char32_t cp);
    void scroll_up();
    void mark_all_dirty();
    void mark_dirty(uint32_t row, uint32_t col);
    bool is_dirty(uint32_t row, uint32_t col) const;
    void clear_dirty(uint32_t row, uint32_t col);

    State state_ = State::Ground;
    GridSize grid_size_{80, 24};
    std::vector<Cell> grid_;
    std::vector<bool> dirty_;
    CursorPosition cursor_{0, 0};
    std::vector<int> csi_params_;
    std::vector<char> osc_buffer_;
};

} // namespace Kaelum