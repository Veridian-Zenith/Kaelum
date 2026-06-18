#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) in vec4 inColor;

layout(binding = 0) sampler2D texSampler;

layout(location = 0) out vec4 fragColor;

void main() {
    float alpha = texture(texSampler, inUV).r;
    fragColor = vec4(inColor.rgb, inColor.a * alpha);
}
