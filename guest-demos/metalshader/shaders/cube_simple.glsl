#version 450

layout(location = 0) in vec2 fragCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform UniformBufferObject {
    vec3 iResolution;
    float iTime;
    vec4 iMouse;
} ubo;

// Simple animated cube shader - no raymarching, just patterns
void main() {
    vec2 uv = (fragCoord - 0.5 * ubo.iResolution.xy) / ubo.iResolution.y;

    // Create rotating cube-like pattern using simple math
    float t = ubo.iTime * 0.5;

    // Rotate UV
    float c = cos(t);
    float s = sin(t);
    vec2 ruv = vec2(
        uv.x * c - uv.y * s,
        uv.x * s + uv.y * c
    );

    // Create cube faces using step functions
    vec2 p = abs(ruv) * 3.0;

    // Determine which "face" we're on based on position
    vec3 col = vec3(0.1, 0.1, 0.15); // Background

    if (p.x < 1.0 && p.y < 1.0) {
        // Front face - changes color over time
        float angle = atan(ruv.y, ruv.x) + t;
        float radius = length(ruv);

        // Color based on angle (simulating cube faces)
        vec3 faceColor;
        if (mod(angle + 3.14159, 6.28318) < 2.094) {
            faceColor = vec3(1.0, 0.0, 0.0); // Red
        } else if (mod(angle + 3.14159, 6.28318) < 4.188) {
            faceColor = vec3(0.0, 1.0, 0.0); // Green
        } else {
            faceColor = vec3(0.0, 0.0, 1.0); // Blue
        }

        // Add brightness variation
        float brightness = 0.6 + 0.4 * sin(ubo.iTime * 2.0);
        col = faceColor * brightness;

        // Add grid
        vec2 grid = fract(p * 5.0);
        if (grid.x > 0.9 || grid.y > 0.9) {
            col = mix(col, vec3(1.0), 0.5);
        }
    }

    fragColor = vec4(col, 1.0);
}
