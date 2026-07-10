#version 450

layout(location = 0) in vec2 inUV;
layout(location = 1) in vec4 inColor;

layout(binding = 0) uniform sampler2D atlas;

layout(location = 0) out vec4 fragColor;

void main() {
    float alpha = texture(atlas, inUV).r;
    fragColor = vec4(inColor.rgb, inColor.a * alpha);
}