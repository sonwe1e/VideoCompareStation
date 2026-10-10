// V01 "panorama + three same-ROI locals": fragment stage of the local-window
// re-sample. `sampleRect` is the normalized sub-rect of the flattened panorama
// snapshot that this window should show (x, y, width, height), computed per slot
// in RoiTripletStrip.qml from the shared ROI, the panorama transform and the panel
// content bounds. Regenerate the checked-in roitripletwindow.frag.qsb after
// editing this file with the Qt the build pins (out/vcpkg x64-windows
// tools/Qt6/bin/qsb.exe), using the qt_add_shaders default target set so every
// RHI backend (d3d11 picks HLSL 50) is covered:
//   qsb.exe --glsl "100 es,120,150" --hlsl 50 --msl 12 -o roitripletwindow.frag.qsb roitripletwindow.frag
#version 440

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    vec4 sampleRect;
    float qt_Opacity;
};

layout(binding = 1) uniform sampler2D src;

void main() {
    fragColor = texture(src, sampleRect.xy + uv * sampleRect.zw) * qt_Opacity;
}
