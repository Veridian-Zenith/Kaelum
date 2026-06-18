#version 450\nlayout(location = 0) in vec2 inPos;\nlayout(location = 0) out vec4 outColor;\nvoid main() { gl_Position = vec4(inPos, 0.0, 1.0); outColor = vec4(0.1, 0.1, 0.1, 1.0); }
