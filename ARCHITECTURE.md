# Architecture Specification: Kaelum

Kaelum is architected as a pipeline of specialized components designed to minimize latency and maximize throughput.

## 1. Loom (The I/O Weaver)
The Loom is responsible for the lifecycle of the Pseudo-Terminal (PTY) and the ingestion of raw data.
- **Asynchronous Core**: Built on `io_uring` to perform non-blocking reads/writes to the PTY master.
- **Zero-Copy Path**: Data is streamed directly from the kernel into pre-allocated ring buffers.
- **Event Loop**: A tight, low-jitter loop that notifies the Nexus when new data is available for parsing.

## 2. Nexus (The Core Emulator)
The Nexus transforms the raw byte stream from the Loom into a structured grid of cells.
- **C++26 State Machine**: A deterministic finite automaton (DFA) implemented with `std::expected` for error handling.
- **O(1) Dispatch**: Uses a specialized jump table for ANSI escape sequences to ensure constant-time processing.
- **Grid Buffer**: A double-buffered cell matrix that tracks characters, colors, and styles.

## 3. Sigil (The Renderer)
The Sigil translates the Nexus grid into visual pixels on the screen.
- **Vulkan 1.3 & Intel Level Zero**: Combining Vulkan's rendering pipeline with Intel Level Zero for low-level GPU memory management and compute acceleration.
- **Native Wayland**: Direct communication with the Wayland compositor for minimal input-to-photon latency.


## 4. Aura (The Aesthetic Layer)
Aura provides the visual polish and high-end design.
- **Shader-Based Effects**: Implements Gaussian blur, bloom, and particle systems via custom GLSL/SPIR-V shaders.
- **Fluid Animations**: 144Hz+ interpolated transitions for terminal resizing and theme switching.
- **Fish-Sensing**: A specialized hook that optimizes rendering for Fish shell's unique output patterns.

## 🔒 Security & Integrity
- **Capability Dropping**: The process starts with necessary privileges to create a PTY and then surgically drops all unnecessary capabilities.
- **Memory Safety**: Strict adherence to C++26 RAII and `std::span` to prevent buffer overflows.
