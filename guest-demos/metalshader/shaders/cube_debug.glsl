#version 450

layout(location = 0) in vec2 fragCoord;
layout(location = 0) out vec4 fragColor;

layout(binding = 0) uniform UniformBufferObject {
    vec3 iResolution;
    float iTime;
    vec4 iMouse;
} ubo;

// Simplified cube for debugging - progressively test each feature

mat4 rotationMatrix(vec3 axis, float angle) {
    axis = normalize(axis);
    float s = sin(angle);
    float c = cos(angle);
    float oc = 1.0 - c;

    return mat4(oc * axis.x * axis.x + c,           oc * axis.x * axis.y - axis.z * s,  oc * axis.z * axis.x + axis.y * s,  0.0,
                oc * axis.x * axis.y + axis.z * s,  oc * axis.y * axis.y + c,           oc * axis.y * axis.z - axis.x * s,  0.0,
                oc * axis.z * axis.x - axis.y * s,  oc * axis.y * axis.z + axis.x * s,  oc * axis.z * axis.z + c,           0.0,
                0.0,                                0.0,                                0.0,                                1.0);
}

float boxIntersect(vec3 ro, vec3 rd, vec3 boxSize) {
    vec3 m = 1.0 / (rd + vec3(1e-8));
    vec3 n = m * ro;
    vec3 k = abs(m) * boxSize;
    vec3 t1 = -n - k;
    vec3 t2 = -n + k;
    float tN = max(max(t1.x, t1.y), t1.z);
    float tF = min(min(t2.x, t2.y), t2.z);
    if (tN > tF || tF < 0.0) return -1.0;
    return tN;
}

void main() {
    vec2 uv = (fragCoord - 0.5 * ubo.iResolution.xy) / ubo.iResolution.y;

    // Camera setup
    vec3 ro = vec3(0.0, 0.0, 5.0);
    vec3 rd = normalize(vec3(uv, -1.5));

    // Rotate
    float angle = ubo.iTime;
    mat4 rot = rotationMatrix(normalize(vec3(1.0, 1.0, 0.0)), angle);
    ro = (rot * vec4(ro, 1.0)).xyz;
    rd = normalize((rot * vec4(rd, 0.0)).xyz);

    // Cube intersection
    vec3 boxSize = vec3(0.8);
    float t = boxIntersect(ro, rd, boxSize);

    vec3 col = vec3(0.1, 0.1, 0.15); // Brighter background for debugging

    if (t > 0.0) {
        vec3 pos = ro + rd * t;

        // Simple solid color first - test if intersection works
        col = vec3(1.0, 0.5, 0.0); // Bright orange

        // Add simple position-based color variation
        vec3 normPos = clamp((pos / boxSize) * 0.5 + 0.5, 0.0, 1.0);
        col = normPos; // Should show RGB gradient if working
    }

    fragColor = vec4(col, 1.0);
}
