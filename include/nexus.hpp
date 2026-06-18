#pragma once

#include <expected>
#include <vector>
#include <span>
#include "common.hpp"

namespace Kaelum {

    enum class NexusError {
        ParseError,
        BufferOverflow
    };

    /**
     * @brief Nexus is the terminal emulator state machine.
     */
    class Nexus {
    public:
        Nexus();
        ~Nexus() = default;

        /**
         * @brief Processes a chunk of bytes from the Loom and updates the grid.
         */
        std::expected<void, NexusError> process_input(std::span<const uint8_t> data);

        /**
         * @brief Returns the current state of the terminal grid.
         */
        const Grid& get_grid() const { return grid_; }

    private:
        Grid grid_;
        size_t cursor_x_ = 0;
        size_t cursor_y_ = 0;
        // State machine internals...
    };

} // namespace Kaelum
