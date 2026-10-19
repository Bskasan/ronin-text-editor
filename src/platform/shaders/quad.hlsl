// quad.hlsl — one pipeline for everything: instanced quads.
// The vertex shader builds the four corners of a triangle strip from SV_VertexID;
// the only vertex buffer is the per-instance buffer.

cbuffer Globals : register(b0) {
    float2 viewport_size; // pixels
    float2 pad;
};

Texture2D<float4> atlas : register(t0); // RGB = ClearType coverage

struct VSIn {
    float4 rect  : RECT;  // x0, y0, x1, y1 in pixels
    float4 uv    : UV;    // u0, v0, u1, v1 in atlas texels (glyphs only)
    float4 color : COLOR; // straight alpha
    uint   kind  : KIND;  // 0 = solid color, 1 = glyph
    uint   vid   : SV_VertexID;
};

struct PSIn {
    float4 pos   : SV_Position;
    float2 uv    : UV;
    float4 color : COLOR;
    nointerpolation uint kind : KIND;
};

struct PSOut {
    float4 color  : SV_Target0; // blended with SRC1_COLOR / INV_SRC1_COLOR
    float4 weight : SV_Target1; // per-channel blend weights
};

PSIn vs_main(VSIn input) {
    float2 corner = float2((float)(input.vid & 1), (float)(input.vid >> 1));
    float2 p = lerp(input.rect.xy, input.rect.zw, corner);

    PSIn output;
    output.pos   = float4(p / viewport_size * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv    = lerp(input.uv.xy, input.uv.zw, corner);
    output.color = input.color;
    output.kind  = input.kind;
    return output;
}

PSOut ps_main(PSIn input) {
    PSOut output;
    output.color = input.color;
    if (input.kind == 1) {
        // Glyph quads are 1:1 with atlas texels, so the interpolated coordinate at the pixel
        // center floors to exactly one texel: no sampler, no filtering.
        float3 coverage = atlas.Load(int3(floor(input.uv), 0)).rgb;
        output.weight = float4(coverage * input.color.a, input.color.a);
    } else {
        output.weight = input.color.aaaa;
    }
    return output;
}
