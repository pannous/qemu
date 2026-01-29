#version 450

// metalshader provides this at binding 0
layout(binding = 0) uniform ShaderToyUBO {
    vec3 iResolution;
    float iTime;
    vec4 iMouse;
} ubo;

layout(location = 0) out vec3 fragColor;

void main() {
    // Generate fullscreen quad from 6 vertices (2 triangles)
    vec2 positions[6] = vec2[](
        vec2(-1, -1), vec2( 1, -1), vec2( 1,  1),  // Triangle 1
        vec2(-1, -1), vec2( 1,  1), vec2(-1,  1)   // Triangle 2
    );

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);

    // Color based on position with time animation
    vec2 uv = positions[gl_VertexIndex] * 0.5 + 0.5;
    float t = ubo.iTime;
    fragColor = vec3(
        0.5 + 0.5 * sin(uv.x * 3.0 + t),
        0.5 + 0.5 * sin(uv.y * 3.0 + t * 1.3),
        0.5 + 0.5 * sin((uv.x + uv.y) * 3.0 + t * 0.7)
    );
}
