#pragma once

#include "common.hpp"
#include "subprocess.hpp"
#include "glyph_engine.hpp"
#include <expected>
#include <string>
#include <vector>
#include <span>
#include <functional>
#include <cstdint>

namespace Kaelum {

struct TermioConfig {
    uint32_t cols = 80;
    uint32_t rows = 24;
    uint32_t width_px = 800;
    uint32_t height_px = 600;
};

enum class TermioError {
    SubprocessFailed,
    PtyReadFailed,
    PtyWriteFailed,
    PollFailed,
};

using DataCallback = std::function<void(std::span<const char> data)>;

class Termio {
public:
    Termio() = default;
    ~Termio();

    Termio(const Termio&) = delete;
    Termio& operator=(const Termio&) = delete;
    Termio(Termio&&) = delete;
    Termio& operator=(Termio&&) = delete;

    std::expected<void, TermioError> init(const TermioConfig& config);
    void deinit();

    std::expected<void, TermioError> write_input(std::span<const char> data);
    std::expected<void, TermioError> process_io();
    std::expected<void, TermioError> resize(uint32_t cols, uint32_t rows);

    void set_data_callback(DataCallback cb) { data_cb_ = std::move(cb); }
    int pty_fd() const { return subprocess_.master_fd(); }
    bool initialized() const { return subprocess_.initialized(); }

private:
    Subprocess subprocess_;
    DataCallback data_cb_;
    bool initialized_ = false;
};

} // namespace Kaelum