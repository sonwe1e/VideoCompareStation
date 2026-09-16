Texture2D<float> yTexture : register(t0);
Texture2D<float2> uvTexture : register(t1);
Texture2D<float> yTextureB : register(t2);
Texture2D<float2> uvTextureB : register(t3);
SamplerState clampSampler : register(s0);

cbuffer Nv12ColorConstants : register(b1) {
    row_major float3x4 yuvToRgb;
};

cbuffer Nv12ColorConstantsB : register(b2) {
    row_major float3x4 yuvToRgbB;
};

cbuffer DifferenceConstants : register(b3) {
    float4 sourceUvRectA : packoffset(c0);
    float4 sourceUvRectB : packoffset(c1);
    float4 planeDimensionsA : packoffset(c2);
    float4 planeDimensionsB : packoffset(c3);
    uint differenceMetric : packoffset(c4.x);
    float differenceGain : packoffset(c4.y);
    uint differenceFilter : packoffset(c4.z);
    float differencePadding : packoffset(c4.w);
    uint thresholdEnabled : packoffset(c5.x);
    float differenceThreshold : packoffset(c5.y);
    uint thresholdPolicy : packoffset(c5.z);
    float thresholdPadding : packoffset(c5.w);
    uint sourceRotationA : packoffset(c6.x);
    uint sourceRotationB : packoffset(c6.y);
    // Crossfade blend amount (0 = A, 1 = B). Unused by absolute metrics.
    float fadeAmount : packoffset(c6.z);
    float rotationPadding : packoffset(c6.w);
};

float2 rotateDisplayUv(float2 uv, uint rotation) {
    if (rotation == 1U) {
        return float2(1.0f - uv.y, uv.x);
    }
    if (rotation == 2U) {
        return 1.0f - uv;
    }
    if (rotation == 3U) {
        return float2(uv.y, 1.0f - uv.x);
    }
    return uv;
}

float4 cubicWeights(float value) {
    const float valueSquared = value * value;
    const float valueCubed = valueSquared * value;
    return float4(
        (-0.5f * value) + valueSquared - (0.5f * valueCubed),
        1.0f - (2.5f * valueSquared) + (1.5f * valueCubed),
        (0.5f * value) + (2.0f * valueSquared) - (1.5f * valueCubed),
        (-0.5f * valueSquared) + (0.5f * valueCubed));
}

float2 regionTexelUv(float2 pixel, float2 dimensions, float4 region) {
    const float2 normalized = (clamp(pixel, 0.0f, dimensions - 1.0f) + 0.5f) / dimensions;
    return region.xy + (normalized * region.zw);
}

float sampleBicubicY(
    Texture2D<float> textureValue, float2 uv, float2 dimensions, float4 region) {
    const float2 position = (saturate(uv) * dimensions) - 0.5f;
    const float2 base = floor(position);
    const float4 weightsX = cubicWeights(position.x - base.x);
    const float4 weightsY = cubicWeights(position.y - base.y);
    float result = 0.0f;
    [unroll]
    for (int row = 0; row < 4; ++row) {
        [unroll]
        for (int column = 0; column < 4; ++column) {
            const float2 pixel = base + float2(column - 1, row - 1);
            result += textureValue.SampleLevel(
                          clampSampler, regionTexelUv(pixel, dimensions, region), 0.0f) *
                      weightsX[column] * weightsY[row];
        }
    }
    return result;
}

float2 sampleBicubicUv(
    Texture2D<float2> textureValue, float2 uv, float2 dimensions, float4 region) {
    const float2 position = (saturate(uv) * dimensions) - 0.5f;
    const float2 base = floor(position);
    const float4 weightsX = cubicWeights(position.x - base.x);
    const float4 weightsY = cubicWeights(position.y - base.y);
    float2 result = 0.0f;
    [unroll]
    for (int row = 0; row < 4; ++row) {
        [unroll]
        for (int column = 0; column < 4; ++column) {
            const float2 pixel = base + float2(column - 1, row - 1);
            result += textureValue.SampleLevel(
                          clampSampler, regionTexelUv(pixel, dimensions, region), 0.0f) *
                      weightsX[column] * weightsY[row];
        }
    }
    return result;
}

float3 sampleYuvA(float2 uv) {
    uv = rotateDisplayUv(uv, sourceRotationA);
    float y;
    float2 chroma;
    if (differenceFilter == 2U) {
        y = sampleBicubicY(yTexture, uv, planeDimensionsA.xy, sourceUvRectA);
        chroma = sampleBicubicUv(uvTexture, uv, planeDimensionsA.zw, sourceUvRectA);
    } else {
        const float2 textureUv = sourceUvRectA.xy + (saturate(uv) * sourceUvRectA.zw);
        y = yTexture.SampleLevel(clampSampler, textureUv, 0.0f);
        chroma = uvTexture.SampleLevel(clampSampler, textureUv, 0.0f);
    }
    return float3(y, chroma);
}

float3 sampleYuvB(float2 uv) {
    uv = rotateDisplayUv(uv, sourceRotationB);
    float y;
    float2 chroma;
    if (differenceFilter == 2U) {
        y = sampleBicubicY(yTextureB, uv, planeDimensionsB.xy, sourceUvRectB);
        chroma = sampleBicubicUv(uvTextureB, uv, planeDimensionsB.zw, sourceUvRectB);
    } else {
        const float2 textureUv = sourceUvRectB.xy + (saturate(uv) * sourceUvRectB.zw);
        y = yTextureB.SampleLevel(clampSampler, textureUv, 0.0f);
        chroma = uvTextureB.SampleLevel(clampSampler, textureUv, 0.0f);
    }
    return float3(y, chroma);
}

float3 sampleRgbA(float3 yuv) {
    return mul(yuvToRgb, float4(yuv, 1.0f));
}

float3 sampleRgbB(float3 yuv) {
    return mul(yuvToRgbB, float4(yuv, 1.0f));
}

float commonBt709Luma(float3 rgb) {
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

float2 commonBt709Chroma(float3 rgb) {
    const float luma = commonBt709Luma(rgb);
    return float2((rgb.b - luma) / 1.8556f, (rgb.r - luma) / 1.5748f);
}

float3 heatmap(float value) {
    const float normalized = saturate(value);
    return float3(
        saturate(normalized * 3.0f),
        saturate((normalized * 3.0f) - 1.0f),
        saturate((normalized * 3.0f) - 2.0f));
}

float4 Nv12PixelShader(
    // Keep the complete reflected register prefix shared with ComposeVertexShader.
    float4 position : SV_POSITION,
    nointerpolation float opacity : TEXCOORD0,
    float2 uv : TEXCOORD1) : SV_TARGET {
    const float y = yTexture.Sample(clampSampler, uv);
    const float2 chroma = uvTexture.Sample(clampSampler, uv);
    const float3 rgb = mul(yuvToRgb, float4(y, chroma.x, chroma.y, 1.0f));
    const float alpha = saturate(opacity);
    return float4(saturate(rgb) * alpha, alpha);
}

float4 DifferencePixelShader(
    // Keep the complete reflected register prefix shared with ComposeVertexShader.
    float4 position : SV_POSITION,
    nointerpolation float opacity : TEXCOORD0,
    float2 uv : TEXCOORD1) : SV_TARGET {
    const float3 yuvA = sampleYuvA(uv);
    const float3 yuvB = sampleYuvB(uv);
    const float3 rgbA = sampleRgbA(yuvA);
    const float3 rgbB = sampleRgbB(yuvB);
    const float3 channelDifference = abs(rgbA - rgbB);
    if (thresholdEnabled != 0U) {
        const float3 thresholdDifference =
            differenceMetric == 4U ? abs(yuvA - yuvB) : channelDifference;
        float thresholdSample;
        if (thresholdPolicy == 0U) {
            thresholdSample = differenceMetric == 4U
                                  ? thresholdDifference.x
                                  : abs(commonBt709Luma(rgbA) - commonBt709Luma(rgbB));
        } else if (thresholdPolicy == 2U) {
            thresholdSample = min(
                thresholdDifference.r, min(thresholdDifference.g, thresholdDifference.b));
        } else {
            thresholdSample = max(
                thresholdDifference.r, max(thresholdDifference.g, thresholdDifference.b));
        }
        if (thresholdSample < differenceThreshold) {
            const float alpha = saturate(opacity);
            return float4(0.0f, 0.0f, 0.0f, alpha);
        }
    }
    float3 result;
    if (differenceMetric == 4U) {
        result = differenceGain * abs(yuvA - yuvB);
    } else if (differenceMetric == 1U) {
        const float difference =
            differenceGain * abs(commonBt709Luma(rgbA) - commonBt709Luma(rgbB));
        result = difference.xxx;
    } else if (differenceMetric == 2U) {
        const float2 difference =
            differenceGain * abs(commonBt709Chroma(rgbA) - commonBt709Chroma(rgbB));
        result = float3(difference.y, 0.0f, difference.x);
    } else if (differenceMetric == 3U) {
        const float3 difference = differenceGain * channelDifference;
        result = heatmap(max(difference.r, max(difference.g, difference.b)));
    } else if (differenceMetric == 5U) {
        // Signed subtract: mid-gray is zero; brighter = A > B, darker = A < B.
        const float3 signedDifference = differenceGain * (rgbA - rgbB);
        result = saturate(0.5f.xxx + signedDifference);
    } else if (differenceMetric == 6U) {
        // Highlight: keep A and tint regions whose max-channel delta exceeds the threshold.
        const float peak = differenceGain * max(channelDifference.r,
                                                 max(channelDifference.g, channelDifference.b));
        const float strength = saturate(peak);
        const float3 highlightTint = float3(1.0f, 0.15f, 0.35f);
        result = lerp(saturate(rgbA), highlightTint, strength);
    } else if (differenceMetric == 7U) {
        // Crossfade A↔B using fadeAmount.
        result = lerp(saturate(rgbA), saturate(rgbB), saturate(fadeAmount));
    } else {
        result = differenceGain * channelDifference;
    }
    const float alpha = saturate(opacity);
    return float4(saturate(result) * alpha, alpha);
}
