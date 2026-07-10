#pragma once

#include <expected>
#include <vector>
#include <span>
#include <array>
#include <variant>
#include <string>
#include <cstdint>
#include <functional>
#include <deque>
#include "common.hpp"

namespace Kaelum {

enum class NexusError {
    ParseError,
    BufferOverflow
};

enum class State : uint8_t {
    Ground = 0,
    Escape,
    CSI,
    EscapeSkip,
    Osc,
    Dcs,
    Sos,
    Pm,
    Apc,
};

struct OscPayload {
    std::vector<char> data;
};

struct CsiParam {
    std::array<int, 16> params{};
    int param_count = 0;
    char intermediate = 0;
    char final_byte = 0;
};

struct ScreenChar {
    char32_t codepoint = U' ';
    Color fg{255, 255, 255};
    Color bg{0, 0, 0};
    CellFlags flags = CellFlags::None;
    bool dirty = true;
};

struct ScrollbackLine {
    std::vector<ScreenChar> chars;
    uint32_t width = 0;
};

class Nexus {
public:
    using GridUpdatedCallback = std::function<void()>;
    using TitleCallback = std::function<void(std::string)>;

    Nexus();
    ~Nexus();

    void set_size(uint32_t cols, uint32_t rows);
    void process_bytes(std::span<const char> data);

    const ScreenChar& cell_at(uint32_t row, uint32_t col) const;
    uint32_t cols() const { return cols_; }
    uint32_t rows() const { return rows_; }
    uint32_t scrollback_lines() const { return static_cast<uint32_t>(scrollback_.size()); }
    const ScrollbackLine& scrollback_line(uint32_t idx) const { return scrollback_[idx]; }

    const CursorPosition& cursor() const { return cursor_; }
    bool cursor_visible() const { return cursor_visible_; }

    void set_grid_updated_callback(GridUpdatedCallback cb) { grid_cb_ = std::move(cb); }
    void set_title_callback(TitleCallback cb) { title_cb_ = std::move(cb); }

    void mark_all_dirty();
    std::string title() const { return title_; }

private:
    uint32_t cols_ = 80;
    uint32_t rows_ = 24;
    std::vector<ScreenChar> grid_;
    std::deque<ScrollbackLine> scrollback_;
    static constexpr size_t MAX_SCROLLBACK = 10000;

    CursorPosition cursor_{0, 0};
    bool cursor_visible_ = true;

    State state_ = State::Ground;
    CsiParam csi_;
    OscPayload osc_;
    std::vector<char> escape_buf_;

    Color fg_{255, 255, 255};
    Color bg_{0, 0, 0};
    CellFlags flags_ = CellFlags::None;

    std::string title_ = "Kaelum";
    std::vector<char> pending_input_;

    GridUpdatedCallback grid_cb_;
    TitleCallback title_cb_;

    uint32_t idx(uint32_t row, uint32_t col) const;
    void scroll_up(uint32_t lines = 1);
    void new_line();
    void carriage_return();
    void put_char(char32_t c);
    void tab();
    void backspace();
    void erase_in_display(int mode);
    void erase_in_line(int mode);
    void set_cursor(uint32_t row, uint32_t col);
    void set_graphics_mode(int param);
    void dispatch_csi();
    void dispatch_escape(char c);
    void osc_dispatch();
    void handle_c0(char c);
    void handle_printable(char32_t c);
    void process_byte(char c);
};

} // namespace Kaelum
