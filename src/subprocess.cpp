#include "subprocess.hpp"
#include "logger.hpp"
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <pty.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <liburing.h>
#include <unistd.h>

namespace Kaelum {

namespace {
    constexpr uint64_t URING_TAG_READ  = 1;
    constexpr uint64_t URING_TAG_WRITE = 2;
}

Subprocess::~Subprocess() {
    deinit();
}

Kaelum::Expected<void, SubprocessError> Subprocess::init(const SubprocessConfig& config) {
    KAELUM_DEBUG("Initializing Subprocess with shell: {}", config.shell);

    // Open PTY
    int master, slave;
    if (openpty(&master, &slave, nullptr, nullptr, nullptr) == -1) {
        KAELUM_ERROR("Failed to open PTY: {}", strerror(errno));
        return Kaelum::make_unexpected(SubprocessError::PtyOpenFailed);
    }
    pty_master_ = master;

    // Set CLOEXEC on master
    int flags = fcntl(master, F_GETFD);
    if (flags != -1) {
        fcntl(master, F_SETFD, flags | FD_CLOEXEC);
    }

    // Initialize io_uring
    io_uring_params params{};
    if (io_uring_queue_init_params(256, &ring_, &params) < 0) {
        close(master);
        return Kaelum::make_unexpected(SubprocessError::UringInitFailed);
    }

    // Create eventfd for signaling
    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd_ < 0) {
        io_uring_queue_exit(&ring_);
        close(master);
        return Kaelum::make_unexpected(SubprocessError::EventFdFailed);
    }

    // Fork and exec
    pid_t pid = fork();
    if (pid == -1) {
        io_uring_queue_exit(&ring_);
        close(event_fd_);
        close(master);
        return Kaelum::make_unexpected(SubprocessError::ForkFailed);
    }

    if (pid == 0) {
        // Child process
        // Set up slave as stdin/stdout/stderr
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        
        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        close(slave);

        // Terminal attributes
        struct termios term{};
        tcgetattr(STDIN_FILENO, &term);
        term.c_lflag |= ECHO | ICANON | ISIG;
        term.c_oflag |= OPOST | ONLCR;
        tcsetattr(STDIN_FILENO, TCSANOW, &term);

        // Session and controlling terminal
        setsid();
        ioctl(STDIN_FILENO, TIOCSCTTY, 0);

        // Environment
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        // Execute shell
        std::vector<char*> argv;
        for (const auto& arg : config.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(config.shell.c_str(), argv.data());
        _exit(1);
    }

    // Parent
    close(slave);
    child_pid_ = pid;
    initialized_ = true;

    // Submit initial read
    submit_read();

    KAELUM_INFO("Subprocess initialized with PID {}", child_pid_);
    return {};
}

void Subprocess::deinit() {
    if (initialized_) {
        if (child_pid_ > 0) {
            kill(child_pid_, SIGTERM);
            waitpid(child_pid_, nullptr, 0);
        }
        if (pty_master_ >= 0) close(pty_master_);
        if (event_fd_ >= 0) close(event_fd_);
        io_uring_queue_exit(&ring_);
        initialized_ = false;
    }
}

Kaelum::Expected<void, SubprocessError> Subprocess::resize(uint32_t cols, uint32_t rows) {
    if (pty_master_ < 0) return {};
    
    struct winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    if (ioctl(pty_master_, TIOCSWINSZ, &ws) < 0) {
        return Kaelum::make_unexpected(SubprocessError::IoctlFailed);
    }
    return {};
}

void Subprocess::submit_read() {
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

Kaelum::Expected<void, SubprocessError> Subprocess::write(std::span<const char> data) {
    if (!initialized_ || pty_master_ < 0) {
        return Kaelum::make_unexpected(SubprocessError::WriteFailed);
    }

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        return Kaelum::make_unexpected(SubprocessError::WriteFailed);
    }

    auto* buf = new std::vector<char>(data.begin(), data.end());
    io_uring_prep_write(sqe, pty_master_, buf->data(), buf->size(), 0);
    io_uring_sqe_set_data64(sqe, URING_TAG_WRITE | (reinterpret_cast<uint64_t>(buf) << 8));

    io_uring_submit(&ring_);
    return {};
}

Kaelum::Expected<void, SubprocessError> Subprocess::process_io() {
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
                KAELUM_ERROR("PTY write error: {}", strerror(-static_cast<int>(res)));
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
        read(event_fd_, &val, sizeof(val));
    }

    return {};
}

} // namespace Kaelum