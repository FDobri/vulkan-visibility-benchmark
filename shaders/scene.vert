#version 450
layout(set=0, binding=0, std430) readonly buffer Objects { vec4 objects[]; };
layout(set=0, binding=1, std430) readonly buffer Visible { uint ids[]; };
layout(set=0, binding=3, std140) uniform Camera {
    mat4 vp;
    vec4 planes[6];
    vec4 right;
    vec4 up;
    uint count;
} camera;
layout(location=0) out vec3 color;
const vec2 corners[6] = vec2[](vec2(-1,0), vec2(0,-1), vec2(1,0),
    vec2(1,0), vec2(0,1), vec2(-1,0));
void main() {
    uint id = ids[gl_InstanceIndex];
    vec4 object = objects[id];
    vec2 p = corners[gl_VertexIndex];
    vec3 center = object.xyz + object.w * (camera.right.xyz * p.x + camera.up.xyz * p.y);
    gl_Position = camera.vp * vec4(center, 1);
    color = mix(vec3(.045,.16,.42), vec3(.38,.14,.045), float(id % 113u) / 112.0);
}
