#include "loom.hpp"
#include <pty.h>
#include <unistd.h>
#include <termios.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <print>
#include <cerrno>

namespace Kaelum {

namespace {
    constexpr uint64_t URING_TAG_READ  = 1;
    constexpr uint64_t URING_TAG_WRITE = 2;

    int set_cloexec(int fd) {
        int flags = fcntl(fd, F_GETFD);
        if (flags == -1) return -1;
        return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

Loom::Loom() = default;

Loom::~Loom() {
    if (pty_master_ >= 0) close(pty_master_);
    if (event_fd_ >= 0) close(event_fd_);
    if (initialized_) {
        io_uring_queue_exit(&ring_);
    }
}

std::expected<void, LoomError> Loom::init(const std::string& shell) {
    // Create PTY
    int master, slave;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) == -1) {
        return std::unexpected(LoomError::PtyOpenFailed);
    }
    pty_master_ = master;
    set_cloexec(master);

    // Setup eventfd for signaling from uring completions
    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd_ < 0) {
        close(master);
        return std::unexpected(LoomError::EventFdFailed);
    }

    auto uring_res = setup_uring();
    if (!uring_res) {
        close(master);
        close(event_fd_);
        return uring_res;
    }

    if (!spawn_shell(shell.empty() ? "fish" : shell)) {
        return std::unexpected(LoomError::ForkFailed);
    }

    close(slave);
    initialized_ = true;

    // Submit initial read
    submit_read();

    return {};
}

std::expected<void, LoomError> Loom::setup_uring() {
    io_uring_params params{};
    if (io_uring_queue_init_params(256, &ring_, &params) < 0) {
        return std::unexpected(LoomError::UringInitFailed);
    }
    return {};
}

bool Loom::spawn_shell(const std::string& shell) {
    int slave_fd = -1;
    pid_t pid = forkpty(&slave_fd, nullptr, nullptr, nullptr);
    if (pid == -1) return false;

    if (pid == 0) {
        // Child: slave_fd is already set as stdin/stdout/stderr by forkpty

        // Set the terminal attributes
        struct termios term{};
        tcgetattr(STDIN_FILENO, &term);
        term.c_lflag |= ECHO | ICANON | ISIG;
        term.c_oflag |= OPOST | ONLCR;
        tcsetattr(STDIN_FILENO, TCSANOW, &term);

        // Set the session ID
        setsid();
        ioctl(STDIN_FILENO, TIOCSCTTY, 0);

        // Set environment
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        // Execute the shell
        execlp(shell.c_str(), shell.c_str(), nullptr);
        // Fallback to sh
        execlp("/bin/sh", "sh", nullptr);
        _exit(1);
    }

    // Parent: close slave_fd (child owns it now)
    if (slave_fd >= 0) close(slave_fd);

    return true;
}

std::expected<void, LoomError> Loom::write_input(std::span<const char> data) {
    if (!initialized_ || pty_master_ < 0) {
        return std::unexpected(LoomError::WriteFailed);
    }

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return std::unexpected(LoomError::WriteFailed);
    }

    // Copy data since uring needs stable buffer
    auto* buf = new std::vector<char>(data.begin(), data.end());
    io_uring_prep_write(sqe, pty_master_, buf->data(), buf->size(), 0);
    io_uring_sqe_set_data64(sqe, URING_TAG_WRITE | (reinterpret_cast<uint64_t>(buf) << 8));

    io_uring_submit(&ring_);
    return {};
}

std::expected<void, LoomError> Loom::process_io() {
    if (!initialized_) return {};

    io_uring_cqe* cqe;
    unsigned head;
    unsigned count = 0;

    io_uring_for_each_cqe(&ring_, head, cqe) {
        count++;
        uint64_t data = io_uring_cqe_get_data64(cqe);
        uint64_t tag = data & 0xFF;
        int64_t res = cqe->res;

        if (tag == URING_TAG_READ) {
            auto* buf = reinterpret_cast<std::vector<char>*>(data >> 8);
            if (res > 0 && data_cb_) {
                data_cb_(std::span<const char>(buf->data(), static_cast<size_t>(res)));
            }
            delete buf;

            // Re-submit read
            auto* new_buf = new std::vector<char>(READ_BUF_SIZE);
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe) {
                io_uring_prep_read(sqe, pty_master_, new_buf->data(), READ_BUF_SIZE, 0);
                io_uring_sqe_set_data64(sqe, URING_TAG_READ | (reinterpret_cast<uint64_t>(new_buf) << 8));
            } else {
                delete new_buf;
            }
        } else if (tag == URING_TAG_WRITE) {
            auto* buf = reinterpret_cast<std::vector<char>*>(data >> 8);
            delete buf;
            if (res < 0) {
                std::println(stderr, "Loom: write error: {}", strerror(-static_cast<int>(res)));
            }
        }
    }

    io_uring_cq_advance(&ring_, count);

    if (count > 0) {
        io_uring_submit(&ring_);
    }

    // Drain eventfd
    if (event_fd_ >= 0) {
        uint64_t val;
        (void)read(event_fd_, &val, sizeof(val));
    }

    return {};
}

void Loom::submit_read() {
    auto* buf = new std::vector<char>(READ_BUF_SIZE);
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        delete buf;
        return;
    }

    io_uring_prep_read(sqe, pty_master_, buf->data(), READ_BUF_SIZE, 0);
    io_uring_sqe_set_data64(sqe, URING_TAG_READ | (reinterpret_cast<uint64_t>(buf) << 8));
    io_uring_submit(&ring_);
}

void Loom::resize_pty(uint32_t cols, uint32_t rows) {
    if (pty_master_ < 0) return;

    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ioctl(pty_master_, TIOCSWINSZ, &ws);
}

} // namespace Kaelum
