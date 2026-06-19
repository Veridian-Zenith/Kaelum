#pragma once

#include <expected>
#include <vector>
#include <span>
#include <array>
=======
#include <variant>
>>>>>>> f3a208535ac134d49e379d14c6e49e33196c5e79
#include "common.hpp"

namespace Kaelum {

    enum class NexusError {
        ParseError,
        BufferOverflow
    };

    enum class State : uint8_t {
        Ground = 0,
        Escape,
        CSI,
<<<<<<< HEAD
        EscapeSkip,  // Consume one byte after ESC ( ) * +
