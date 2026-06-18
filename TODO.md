# Kaelum Project Roadmap: Path to Public Beta

Kaelum is a high-performance, GPU-accelerated, "natively fish" terminal emulator built with C++26, Vulkan 1.3, and io_uring.

## 🏁 Completed
- [x] **Core Infrastructure**
    - [x] Project scaffolding and isolated CMake/Ninja build system.
    - [x] C++26 toolchain integration (Clang++, mold).
    - [x] OSL v3 Licensing and Architecture documentation.
- [x] **Loom (I/O Layer)**
    - [x] `io_uring` based PTY communication for zero-copy potential.
    - [x] Verified PTY read/write loops.
- [x] **Nexus (Emulation Layer)**
    - [x] ANSI/VT state machine implementation.
    - [x] $O(1)$ dispatch table via member function pointers.
    - [x] Grid-based terminal state management.
- [x] **Sigil (Hardware Interface - Initial)**
    - [x] Vulkan 1.3 instance and device initialization.
    - [x] Intel Level Zero (ze_api.h) driver binding.
    - [x] Native Wayland connection and `xdg_shell` surface mapping.
- [x] **CI/CD**
    - [x] CodeQL security analysis pipeline.
    - [x] Debug/Release build verification with SYCL/Level Zero.

## 🚀 Phase 1: Rendering Foundation (Immediate)
- [ ] **Wayland Event Integration**
    - [ ] Implement `xdg_surface` configure listener.
    - [ ] Handle window resize events to trigger swapchain recreation.
    - [ ] Process `wl_display` events in the main loop.
- [ ] **Vulkan Pipeline Setup**
    - [ ] Create RenderPass and Graphics Pipeline.
    - [ ] Implement Swapchain management (acquire/present).
    - [ ] Write GLSL shaders for glyph rendering.
- [ ] **The Unified Main Loop**
    - [ ] Integrate Loom $\rightarrow$ Nexus $\rightarrow$ Sigil.
    - [ ] Implement high-refresh-rate frame timing.
    - [ ] Transition from `std::this_thread::sleep_for` to event-driven wakeup.

## 🎨 Phase 2: Typography & Aesthetics
- [ ] **Font Engine**
    - [ ] Integrate FreeType for glyph rasterization.
    - [ ] Implement FontConfig for system font discovery.
    - [ ] Build a GPU-resident Glyph Cache (Atlas).
- [ ] **Color & Style**
    - [ ] TrueColor (24-bit) and Xterm-256 support.
    - [ ] Implementation of "Veridian Zenith" aesthetic themes.
    - [ ] Support for bold, italic, and underlined text styles.
- [ ] **Layout**
    - [ ] Dynamic grid resizing.
    - [ ] Padding and margin controls.

## ⚡ Phase 3: Performance & Optimization
- [ ] **Intel XE Optimizations**
    - [ ] Leverage Level Zero for low-overhead GPU memory management.
    - [ ] Implement SYCL kernels for grid-to-vertex buffer transformations.
- [ ] **I/O Refinement**
    - [ ] Multi-threaded PTY polling to decouple emulation from rendering.
    - [ ] Zero-copy data path from `io_uring` to Nexus.
- [ ] **Rendering Latency**
    - [ ] Implement "Mailbox" present mode for tear-free, low-latency output.
    - [ ] Optimize vertex buffer updates using `vkCmdUpdateBuffer` or mapped memory.

## 🛠️ Phase 4: Feature Completion
- [ ] **User Interaction**
    - [ ] Wayland keyboard input binding.
    - [ ] Mouse support (click, scroll, selection).
    - [ ] Copy/Paste buffer integration.
- [ ] **Terminal Capabilities**
    - [ ] Scrollback buffer implementation.
    - [ ] Support for alternate screen buffers (e.g., for vim/htop).
    - [ ] PTY resize signals (`SIGWINCH`).
- [ ] **Configuration**
    - [ ] JSON/TOML configuration file support.
    - [ ] Hot-reloading of themes and fonts.

## 🧪 Phase 5: Beta Stabilization
- [ ] **Stress Testing**
    - [ ] Rapid resize and high-throughput output tests.
    - [ ] Long-running stability audits.
- [ ] **Quality Assurance**
    - [ ] Memory leak detection (ASan/MSan).
    - [ ] Full security audit via CodeQL.
- [ ] **Distribution**
    - [ ] Installation scripts for Linux.
    - [ ] Final documentation for public beta users.
