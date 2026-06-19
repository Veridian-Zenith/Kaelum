#pragma once

#include <expected>
#include <vector>
#include <span>
#include <array>
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
        EscapeSkip,  // Consume one byte after ESC ( ) * +
        OSC,
        Count // Sentinel for dispatch table size
    };

    /**
     * @brief Nexus is the terminal emulator state machine.
     */
    class Nexus {
    public:
        Nexus();
        ~Nexus() = default;

        std::expected<void, NexusError> process_input(std::span<const uint8_t> data);

        void resize(size_t cols, size_t rows);

        const Grid& get_grid() const { return grid_; }
        size_t cols() const { return cols_; }
        size_t rows() const { return rows_; }
        std::pair<size_t, size_t> get_cursor() const { return {cursor_x_, cursor_y_}; }

    private:
        // State handler function pointer for maximum performance
        using StateHandler = void (Nexus::*)(uint8_t);
        std::array<StateHandler, static_cast<size_t>(State::Count)> dispatch_table_;

        // State handlers
        void handle_ground(uint8_t c);
        void handle_escape(uint8_t c);
        void handle_csi(uint8_t c);
        void handle_escape_skip(uint8_t c);
        void handle_osc(uint8_t c);

        // Sequence helpers
        void process_csi(uint8_t final_char);
        void parse_sgr(std::span<const uint8_t> params);

        size_t cols_ = k_default_cols;
        size_t rows_ = k_default_rows;
        Grid grid_;
        size_t cursor_x_ = 0;
        size_t cursor_y_ = 0;
        State current_state_ = State::Ground;
        std::vector<uint8_t> sequence_buffer_;
        char csi_prefix_ = 0;  // '?' for DEC private, '>' for secondary DA, 0 for standard

        void move_cursor(int dx, int dy);
        void scroll_up();
        void set_cell(char32_t cp, Color fg, Color bg, uint32_t attrs);
        void clear_screen();
    };

} // namespace Kaelum
