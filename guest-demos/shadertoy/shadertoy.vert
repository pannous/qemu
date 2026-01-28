#version 450
// Reference vertex shader for shadertoy_viewer.c
// Generates fullscreen quad, passes texture coordinates to fragment shader

layout(location = 0) out vec2 fragCoord;

layout(binding = 0, set = 0) uniform UniformBufferObject {
    vec3 iResolution;  // viewport resolution (in pixels)
    float iTime;       // shader playback time (in seconds)
    vec4 iMouse;       // mouse pixel coords (xy: current, zw: click)
} ubo;

void main() {
    // Generate fullscreen quad (2 triangles = 6 vertices)
    vec2 positions[6] = vec2[](
        vec2(-1.0, -1.0),  // Bottom-left
        vec2(1.0, -1.0),   // Bottom-right
        vec2(1.0, 1.0),    // Top-right
        vec2(-1.0, -1.0),  // Bottom-left
        vec2(1.0, 1.0),    // Top-right
        vec2(-1.0, 1.0)    // Top-left
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    
    // Convert from [-1,1] NDC to [0,resolution] pixel coordinates
    fragCoord = (positions[gl_VertexIndex] * 0.5 + 0.5) * ubo.iResolution.xy;
}
