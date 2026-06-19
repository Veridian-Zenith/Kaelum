#include "loom.hpp"
#include <pty.h>
#include <unistd.h>
#include <termios.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sys/eventfd.h>
#include <fcntl.h>

namespace {
    constexpr uint64_t URING_TAG_READ  = 1;
    constexpr uint64_t URING_TAG_WRITE = 2;
}

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
        // Child: Execute user's preferred shell ($SHELL), fall back to fish, then /bin/sh
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

    // Set non-blocking for poll()+read() pattern in main loop.
    // PTY reads now happen via direct read() instead of io_uring.
    int flags = fcntl(master_fd_, F_GETFL, 0);
    fcntl(master_fd_, F_SETFL, flags | O_NONBLOCK);

    return {};
}

void Loom::submit_read() {
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return;
    io_uring_prep_read(sqe, master_fd_, read_buffer_, k_ring_buffer_size, -1);
    io_uring_sqe_set_data64(sqe, URING_TAG_READ);
    io_uring_submit(&ring_);
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

    unsigned head;
    unsigned count = 0;
    io_uring_for_each_cqe(&ring_, head, cqe) {
        uint64_t tag = io_uring_cqe_get_data64(cqe);
        int res = cqe->res;
        count++;

        if (tag == URING_TAG_READ) {
            io_uring_cq_advance(&ring_, count);
            if (res <= 0) {
                submit_read();
                return (res == 0) ? std::expected<size_t, LoomError>(0)
                                  : std::unexpected(LoomError::ReadFailed);
            }
            size_t bytes_to_copy = std::min(static_cast<size_t>(res), buffer.size());
            std::memcpy(buffer.data(), read_buffer_, bytes_to_copy);
            submit_read();
            return bytes_to_copy;
        }
        // URING_TAG_WRITE completions are silently consumed
    }
    if (count > 0) io_uring_cq_advance(&ring_, count);

    return 0;
}

std::expected<void, LoomError> Loom::write(std::span<const uint8_t> data) {
    if (data.empty()) return {};
    size_t to_write = std::min(data.size(), k_write_buffer_size);
    std::memcpy(write_buffer_, data.data(), to_write);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
    if (!sqe) return std::unexpected(LoomError::WriteFailed);

    io_uring_prep_write(sqe, master_fd_, write_buffer_, to_write, -1);
    io_uring_sqe_set_data64(sqe, URING_TAG_WRITE);
    io_uring_submit(&ring_);

    return {};
}

} // namespace Kaelum
