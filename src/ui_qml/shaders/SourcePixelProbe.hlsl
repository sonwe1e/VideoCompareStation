// Pixel probe shaders: one 1x1 draw reads the exact plane code values behind the same
// shader-resource views the display path uses. Load() is unfiltered; UNORM views return
// sample/max, from which the CPU recovers the exact integer code (see D3d11SourcePixelProbe).
// The array variant is required for zero-copy D3D11VA frames whose views are single-slice
// Texture2DArray views with a non-zero FirstArraySlice; slice 0 inside the view is the slice
// the view was created for.

Texture2D<float> probeLuma2D : register(t0);
Texture2D<float2> probeChroma2D : register(t1);
Texture2DArray<float> probeLumaArray : register(t0);
Texture2DArray<float2> probeChromaArray : register(t1);

cbuffer ProbeTexelConstants : register(b0) {
    uint2 probeTexelLuma;
    uint2 probeTexelChroma;
};

void ProbeVertexShader(uint vertexId : SV_VertexID, out float4 position : SV_POSITION) {
    // One triangle covering the 1x1 viewport; no vertex buffer is bound.
    const float2 corner = float2((vertexId << 1U) & 2U, vertexId & 2U);
    position = float4((corner * 2.0f) - 1.0f, 0.0f, 1.0f);
}

uint4 ProbePixelShader2D(float4 position : SV_POSITION) : SV_TARGET {
    const float luma = probeLuma2D.Load(int3(probeTexelLuma, 0));
    const float2 chroma = probeChroma2D.Load(int3(probeTexelChroma, 0));
    return uint4(asuint(luma), asuint(chroma.x), asuint(chroma.y), 0U);
}

uint4 ProbePixelShader2DArray(float4 position : SV_POSITION) : SV_TARGET {
    const float luma = probeLumaArray.Load(int4(probeTexelLuma, 0, 0));
    const float2 chroma = probeChromaArray.Load(int4(probeTexelChroma, 0, 0));
    return uint4(asuint(luma), asuint(chroma.x), asuint(chroma.y), 0U);
}
