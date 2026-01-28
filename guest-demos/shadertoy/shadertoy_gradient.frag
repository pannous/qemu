#version 450
// Example fragment shader for shadertoy_viewer.c
// Simple animated gradient to demonstrate uniforms

layout(location = 0) in vec2 fragCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0, set = 0) uniform UniformBufferObject {
    vec3 iResolution;  // viewport resolution (in pixels)
    float iTime;       // shader playback time (in seconds)
    vec4 iMouse;       // mouse pixel coords (xy: current, zw: click)
} ubo;

void main() {
    // Normalize pixel coordinates (0.0 to 1.0)
    vec2 uv = fragCoord / ubo.iResolution.xy;
    
    // Animated pulse based on time
    float pulse = abs(sin(ubo.iTime));
    
    // Gradient: red varies with X and pulse, green with Y, blue oscillates
    fragColor = vec4(
        uv.x * pulse,                       // Red: left=0, right=pulse
        uv.y,                               // Green: bottom=0, top=1
        0.5 + 0.5 * sin(ubo.iTime * 2.0),  // Blue: oscillates 0-1
        1.0                                 // Alpha: opaque
    );
}
