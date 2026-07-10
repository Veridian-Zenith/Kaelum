# Kaelum Terminal Emulator - Development Status

## Project Overview
Kaelum is a high-performance, GPU-accelerated terminal emulator built with C++26, Vulkan 1.3, io_uring, and Wayland. It draws inspiration from Ghostty and Kitty.

## Current Architecture

### Core Components
1. **Subprocess** (`src/subprocess.cpp`, `include/subprocess.hpp`) - PTY management with io_uring
   - PTY creation via `openpty()`
   - io_uring for async I/O
   - Fork/exec for shell spawning
   - Currently uses master fd for both parent and child (needs fix)

2. **Termio** (`src/termio.cpp`, `include/termio.hpp`) - Terminal I/O layer
   - Wraps Subprocess
   - Handles PTY read/write
   - Error mapping

3. **Terminal** (`src/terminal.cpp`, `include/terminal.hpp`) - VT100/ANSI emulation
   - State machine for escape sequences
   - Grid-based cell management
   - Basic control sequence handling

4. **GlyphEngine** (`src/glyph_engine.cpp`, `include/glyph_engine.hpp`) - Font rendering
   - FreeType integration
   - Glyph atlas management
   - Currently uses `std::expected` (needs migration to Kaelum::Expected)

5. **Renderer** (`src/renderer.cpp`, `include/renderer.hpp`) - Vulkan rendering
   - Vulkan 1.3 + Wayland surface
   - Glyph atlas texture
   - Shader-based glyph rendering

6. **Surface** (`src/surface.cpp`, `include/surface.hpp`) - Main integration
   - Coordinates Termio + Renderer
   - Wayland surface management

7. **Logger** (`include/logger.hpp`) - Custom logging with source_location

### Build System
- CMake with C++26, Clang, MOLD linker
- Dependencies: Wayland, Vulkan, FreeType, FontConfig, liburing, xkbcommon
- Shader compilation via glslangValidator

## Known Issues (Blocking Build)

### 1. Missing System Headers
- `ft2build.h` - FreeType development headers not found
- `R_OK` - Missing `<unistd.h>` include

### 2. Logger Issues
- `std::source_location` not recognized (C++20 feature, needs C++20+)
- `std::format_string` not recognized
- `std::print` not recognized (C++23 feature)
- Logger uses `std::format_string` which requires C++26 format support

### 3. GlyphEngine Return Types
- Uses `std::expected` but declaration uses `Kaelum::Expected`
- Need to update implementation to use `Kaelum::Expected`

### 4. Subprocess Issues
- `pid_` member not declared in class (should be `child_pid_`)
- Child process PTY setup incomplete (slave fd handling)
- Missing `child_pid_` member in class definition

### 5. Renderer Vulkan Types
- Using `void*` for Vulkan handles instead of proper types
- Need proper Vulkan header includes

### 6. Missing Includes
- `<unistd.h>` for `R_OK`, `access()`
- `<fcntl.h>` for `fcntl()`
- Vulkan headers for proper types

## Next Steps

### Immediate (Fix Build)
1. Add missing includes to all source files
2. Fix GlyphEngine return types to use `Kaelum::Expected`
3. Fix Subprocess `pid_` → `child_pid_` 
4. Add missing system includes
5. Fix logger to work with available C++ features

### Short Term
1. Fix Subprocess PTY slave handling in child
2. Complete Terminal escape sequence handling
3. Implement Renderer with proper Vulkan types
4. Add Wayland event loop to Surface
4. Implement glyph rendering pipeline

### Medium Term
1. Input handling (keyboard/mouse)
2. Scrollback buffer
3. Configuration system
4. Performance optimization

## File Structure
```
Kaelum/
├── CMakeLists.txt
├── include/
│   ├── common.hpp
│   ├── expected.hpp
│   ├── glyph_engine.hpp
│   ├── logger.hpp
│   ├── renderer.hpp
│   ├── surface.hpp
│   ├── subprocess.hpp
│   ├── termio.hpp
│   └── terminal.hpp
├── src/
│   ├── glyph_engine.cpp
    ├── logger.hpp (header only)
    ├── main.cpp
    ├── renderer.cpp
    ├── surface.cpp
    ├── subprocess.cpp
    ├── surface.cpp
    ├── termio.cpp
    ├── terminal.cpp
    └── xdg-shell-client-protocol.cpp
├── shaders/
│   ├── glyph.vert
│   ├── glyph.frag
│   ├── glyph.vert.spv
│   └── glyph.frag.spv
└── build/
```

## Building
```bash
cd /home/dae/Work/Kaelum
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j$(nproc)
```

## Testing
```bash
./build/kaelum
```

## References
- Ghostty source: `/home/dae/Work/ghostty/`
- Vulkan spec: https://vulkan.lunarg.com/doc/view/latest
- Wayland protocol: https://wayland.freedesktop.org/docs/html/
- io_uring: https://kernel.dk/io_uring.pdf