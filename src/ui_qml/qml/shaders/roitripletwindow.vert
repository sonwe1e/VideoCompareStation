// V01 "panorama + three same-ROI locals": vertex stage of the local-window re-sample.
// The cell quad's 0..1 texture coordinates are passed through as `uv`; the fragment
// stage remaps them into the per-window sub-rect of the flattened panorama snapshot.
//
// Qt 6 ShaderEffect only loads preprocessed .qsb packages (inline GLSL strings are
// resolved as URLs and fail with "Failed to deserialize QShader"). Regenerate the
// checked-in roitripletwindow.vert.qsb after editing this file with the Qt the build
// pins (out/vcpkg x64-windows tools/Qt6/bin/qsb.exe), using the qt_add_shaders
// default target set so every RHI backend (d3d11 picks HLSL 50) is covered:
//   qsb.exe --glsl "100 es,120,150" --hlsl 50 --msl 12 -o roitripletwindow.vert.qsb roitripletwindow.vert
#version 440

layout(location = 0) in vec4 qt_Vertex;
layout(location = 1) in vec2 qt_MultiTexCoord0;
layout(location = 0) out vec2 uv;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
};

void main() {
    uv = qt_MultiTexCoord0;
    gl_Position = qt_Matrix * qt_Vertex;
}
