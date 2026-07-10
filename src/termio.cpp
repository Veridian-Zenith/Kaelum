#include "termio.hpp"
#include "subprocess.hpp"
#include "logger.hpp"
#include <expected>

namespace Kaelum {

Termio::~Termio() {
    deinit();
}

std::expected<void, TermioError> Termio::init(const TermioConfig& config) {
    KAELUM_DEBUG("Initializing Termio with {}x{}", config.cols, config.rows);
    
    SubprocessConfig proc_config;
    proc_config.cols = config.cols;
    proc_config.rows = config.rows;
    proc_config.width_px = config.width_px;
    proc_config.height_px = config.height_px;
    proc_config.shell = "sh";
    proc_config.args = {"sh"};
    
    auto res = subprocess_.init(proc_config);
    if (!res) {
        KAELUM_ERROR("Failed to init subprocess");
        return std::unexpected(TermioError::SubprocessFailed);
    }

    subprocess_.set_data_callback([this](std::span<const char> data) {
        if (data_cb_) data_cb_(data);
    });

    initialized_ = true;
    KAELUM_INFO("Termio initialized");
    return {};
}

void Termio::deinit() {
    if (initialized_) {
        subprocess_.deinit();
        initialized_ = false;
    }
}

std::expected<void, TermioError> Termio::write_input(std::span<const char> data) {
    if (!initialized_) return std::unexpected(TermioError::PtyWriteFailed);
    auto res = subprocess_.write(data);
    if (!res) {
        return std::unexpected(TermioError::PtyWriteFailed);
    }
    return {};
}

std::expected<void, TermioError> Termio::process_io() {
    if (!initialized_) return {};
    auto res = subprocess_.process_io();
    if (!res) {
        // Map SubprocessError to TermioError
        switch (res.error()) {
            case SubprocessError::ReadFailed:
                return std::unexpected(TermioError::PtyReadFailed);
            case SubprocessError::WriteFailed:
                return std::unexpected(TermioError::PtyWriteFailed);
            case SubprocessError::UringInitFailed:
                return std::unexpected(TermioError::PollFailed);
            default:
                return std::unexpected(TermioError::PtyReadFailed);
        }
    }
    return {};
}

std::expected<void, TermioError> Termio::resize(uint32_t cols, uint32_t rows) {
    if (!initialized_) return std::unexpected(TermioError::PtyWriteFailed);
    auto res = subprocess_.resize(cols, rows);
    if (!res) {
        return std::unexpected(TermioError::PtyWriteFailed);
    }
    return {};
}

} // namespace Kaelum