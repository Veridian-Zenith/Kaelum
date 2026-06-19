#pragma once

#include <expected>
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <liburing.h>
#include <sys/ioctl.h>
#include "common.hpp"


namespace Kaelum {

    enum class LoomError {
        PtyCreationFailed,
        UringInitFailed,
        ReadFailed,
        WriteFailed,
        ForkFailed
    };

    /**
     * @brief Loom handles the PTY lifecycle and asynchronous I/O via io_uring.
     */
    class Loom {
    public:
        Loom();
        ~Loom();

        // Delete copy/move to ensure resource safety
        Loom(const Loom&) = delete;
        Loom& operator=(const Loom&) = delete;

        /**
         * @brief Initializes the PTY and io_uring ring.
         */
        std::expected<void, LoomError> initialize();

        /**
         * @brief Reads available data from the PTY into the provided buffer.
         */
        std::expected<size_t, LoomError> poll_read(std::span<uint8_t> buffer);

        /**
         * @brief Writes data to the PTY.
         */
        std::expected<void, LoomError> write(std::span<const uint8_t> data);

        /**
         * @brief Returns the PTY master file descriptor.
         */
        int master_fd() const { return master_fd_; }

        /**
         * @brief Registers a wake FD for io_uring completions.
         */
        std::expected<int, LoomError> register_wake_fd();

        /**
         * @brief Sets the PTY window size (TIOCSWINSZ).
         */
        void set_pty_size(uint16_t cols, uint16_t rows, uint16_t xpixel = 0, uint16_t ypixel = 0) {
            struct winsize ws = {rows, cols, xpixel, ypixel};
            if (master_fd_ >= 0) ioctl(master_fd_, TIOCSWINSZ, &ws);
        }

    private:
        void submit_read();

        int master_fd_ = -1;
        pid_t child_pid_ = -1;
        struct io_uring ring_;
        bool initialized_ = false;

        static constexpr size_t k_ring_buffer_size = 4096;
        static constexpr size_t k_write_buffer_size = 256;
        uint8_t read_buffer_[k_ring_buffer_size];
        uint8_t write_buffer_[k_write_buffer_size];
    };

} // namespace Kaelum
