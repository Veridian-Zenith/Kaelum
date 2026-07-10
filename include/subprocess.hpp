#pragma once

#include "common.hpp"
#include "expected.hpp"
#include <string>
#include <vector>
#include <span>
#include <functional>
#include <cstdint>
#include <pty.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <signal.h>
#include <liburing.h>

namespace Kaelum {

struct PtyFds {
    int master = -1;
    int slave = -1;
};

struct SubprocessConfig {
    std::string shell = "sh";
    std::vector<std::string> args = {"sh"};
    std::string cwd;
    std::vector<std::pair<std::string, std::string>> env;
    uint32_t cols = 80;
    uint32_t rows = 24;
    uint32_t width_px = 800;
    uint32_t height_px = 600;
};

enum class SubprocessError {
    PtyOpenFailed,
    UringInitFailed,
    ForkFailed,
    ExecFailed,
    EventFdFailed,
    WriteFailed,
    ReadFailed,
    IoctlFailed,
};

class Subprocess {
public:
    Subprocess() = default;
    ~Subprocess();

    Subprocess(const Subprocess&) = delete;
    Subprocess& operator=(const Subprocess&) = delete;
    Subprocess(Subprocess&&) = delete;
    Subprocess& operator=(Subprocess&&) = delete;

    Kaelum::Expected<void, SubprocessError> init(const SubprocessConfig& config = {});
    void deinit();

    Kaelum::Expected<void, SubprocessError> write(std::span<const char> data);
    Kaelum::Expected<void, SubprocessError> process_io();
    Kaelum::Expected<void, SubprocessError> resize(uint32_t cols, uint32_t rows);

    int master_fd() const { return pty_master_; }
    bool initialized() const { return initialized_; }

    void set_data_callback(std::function<void(std::span<const char>)> cb) { data_cb_ = std::move(cb); }

private:
    int pty_master_ = -1;
    int event_fd_ = -1;
    io_uring ring_{};
    pid_t child_pid_ = -1;
    bool initialized_ = false;
    std::function<void(std::span<const char>)> data_cb_;

    static constexpr size_t READ_BUF_SIZE = 65536;

    void submit_read();
    Kaelum::Expected<void, SubprocessError> setup_uring();
    Kaelum::Expected<void, SubprocessError> setup_pty(const std::string& shell,
                                                      const std::vector<std::string>& args,
                                                      const std::string& cwd,
                                                      const std::vector<std::pair<std::string, std::string>>& env);
    void cleanup_child();
};

} // namespace Kaelum