#include "loom.hpp"
#include <pty.h>
#include <unistd.h>
#include <termios.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sys/eventfd.h>

namespace Kaelum {

Loom::Loom() {
    // Zero out the ring for safety
    std::memset(&ring_, 0, sizeof(ring_));
}

Loom::~Loom() {
    if (master_fd_ != -1) {
        close(master_fd_);
    }
    if (initialized_) {
        io_uring_queue_exit(&ring_);
    }
}



std::expected<void, LoomError> Loom::initialize() {
    // 1. Fork PTY
    child_pid_ = forkpty(&master_fd_, nullptr, nullptr, nullptr);
    
    if (child_pid_ == -1) {
        return std::unexpected(LoomError::ForkFailed);
    }

    if (child_pid_ == 0) {
        // Child: Execute the user's preferred shell, with Fish as first preference
        const char* shell = std::getenv("SHELL");
        if (!shell || shell[0] == '\0') shell = "/usr/bin/fish";
        execl(shell, shell, nullptr);
        // If execl fails, try /bin/sh as last resort
        execl("/bin/sh", "sh", nullptr);
        std::exit(1);
    }

    // Parent: Initialize io_uring
    if (io_uring_queue_init(32, &ring_, 0) < 0) {
        return std::unexpected(LoomError::UringInitFailed);
    }
    initialized_ = true;

    // Set master_fd to non-blocking for safety

    int flags = fcntl(master_fd_, F_GETFL, 0);
    fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);

    // Submit initial read request
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return std::unexpected(LoomError::ReadFailed);

    io_uring_prep_read(sqe, master_fd_, read_buffer_, k_ring_buffer_size, 0);
    io_uring_submit(&ring_);

    return {};
}

std::expected<int, LoomError> Loom::register_wake_fd() {
    int wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd < 0) {
        return std::unexpected(LoomError::UringInitFailed);
    }
    if (io_uring_register_eventfd(&ring_, wake_fd) < 0) {
        close(wake_fd);
        return std::unexpected(LoomError::UringInitFailed);
    }
    return wake_fd;
}

std::expected<size_t, LoomError> Loom::poll_read(std::span<uint8_t> buffer) {
    struct io_uring_cqe *cqe;
    
    // Non-blocking peek at the completion queue
    if (io_uring_peek_cqe(&ring_, &cqe) == 0) {
        int res = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);

        if (res < 0) {
            return std::unexpected(LoomError::ReadFailed);
        }

        if (res > 0) {
            size_t bytes_to_copy = std::min(static_cast<size_t>(res), buffer.size());
            std::memcpy(buffer.data(), read_buffer_, bytes_to_copy);
            
            // Re-submit read request
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                io_uring_prep_read(sqe, master_fd_, read_buffer_, k_ring_buffer_size, 0);
                io_uring_submit(&ring_);
            }
            
            return bytes_to_copy;
        }
    }

    return 0; // No data available
}

std::expected<void, LoomError> Loom::write(std::span<const uint8_t> data) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return std::unexpected(LoomError::WriteFailed);

    io_uring_prep_write(sqe, master_fd_, data.data(), data.size(), 0);
    io_uring_submit(&ring_);
    
    // For writes in a terminal, we typically want to ensure they are sent
    // We'll wait for this specific CQE to avoid overfilling the ring
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&ring_, &cqe) < 0) {
        return std::unexpected(LoomError::WriteFailed);
    }
    
    int res = cqe->res;
    io_uring_cqe_seen(&ring_, cqe);
    
    if (res < 0) return std::unexpected(LoomError::WriteFailed);
    
    return {};
}

} // namespace Kaelum
