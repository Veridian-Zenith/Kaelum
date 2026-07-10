#pragma once

#include <expected>
#include <string>
#include <vector>
#include <memory>
#include <span>
#include <functional>
#include <cstdint>
#include <liburing.h>
#include <sys/ioctl.h>

namespace Kaelum {

enum class LoomError {
    PtyOpenFailed,
    UringInitFailed,
    ForkFailed,
    EventFdFailed,
    WriteFailed,
    ReadFailed,
};

class Loom {
public:
    using DataCallback = std::function<void(std::span<const char> data)>;

    Loom();
    ~Loom();

    Loom(const Loom&) = delete;
    Loom& operator=(const Loom&) = delete;
    Loom(Loom&&) = delete;
    Loom& operator=(Loom&&) = delete;

    std::expected<void, LoomError> init(const std::string& shell = "");
    std::expected<void, LoomError> write_input(std::span<const char> data);
    std::expected<void, LoomError> process_io();

    void set_data_callback(DataCallback cb) { data_cb_ = std::move(cb); }
    int pty_fd() const { return pty_master_; }
    int event_fd() const { return event_fd_; }
    bool is_initialized() const { return initialized_; }

    void resize_pty(uint32_t cols, uint32_t rows);

private:
    int pty_master_ = -1;
    int event_fd_ = -1;
    io_uring ring_{};
    bool initialized_ = false;
    DataCallback data_cb_;

    static constexpr size_t READ_BUF_SIZE = 65536;

    void submit_read();

    std::expected<void, LoomError> setup_uring();
    bool spawn_shell(const std::string& shell);
};

} // namespace Kaelum
