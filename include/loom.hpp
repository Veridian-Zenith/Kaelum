#pragma once

#include <expected>
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <liburing.h>
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

    private:
        int master_fd_ = -1;
        pid_t child_pid_ = -1;
        struct io_uring ring_;
        bool initialized_ = false;
        
        // Buffer for io_uring read requests

        static constexpr size_t k_ring_buffer_size = 4096;
        uint8_t read_buffer_[k_ring_buffer_size];
    };

} // namespace Kaelum
