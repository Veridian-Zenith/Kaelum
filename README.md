# Kaelum

**Kaelum** is a high-performance, GPU-accelerated terminal emulator engineered for absolute efficiency, surgical system integration, and zenith-level aesthetics. 

Designed from the ground up to be the definitive terminal for the **Fish shell**, Kaelum bypasses traditional bottlenecks to provide a zero-latency interface between the user and the machine.

## 🌌 Philosophy

Kaelum is built on three pillars:
1. **Absolute Performance**: Utilizing `io_uring` for I/O and `Vulkan 1.3` for rendering to ensure that the hardware is the only limiting factor.
2. **Surgical Precision**: Low-level integration with the Linux kernel and Wayland protocol, avoiding bloated abstractions.
3. **Zenith Design**: A visual experience that mirrors the elegance and fluidity of the Veridian Zenith identity.

## 🛠 Tech Stack

- **Language**: C++26 (Clang++ / mold)
- **I/O**: `io_uring`
- **Graphics**: Vulkan 1.3
- **Windowing**: Native Wayland
- **Typography**: FreeType + HarfBuzz
- **Build System**: CMake + Ninja

## 🏗 Architecture

- **Loom**: The asynchronous I/O weaver handling PTY communication via `io_uring`.
- **Nexus**: The high-speed ANSI/VT state machine for sequence parsing.
- **Sigil**: The GPU-resident glyph renderer for flicker-free, high-refresh-rate output.
- **Aura**: The aesthetic layer managing shaders, blur, and fluid animations.

---
© 2026 Veridian Zenith | Architected by Dae Euhwa <daedaevibin@ik.me>
