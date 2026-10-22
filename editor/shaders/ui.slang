struct Push {
    float4 rect;
    float4 view;
    uint4 data;
    uint4 track;
    float4 screen;
    float4 color;
    float playhead;
};
[[vk::push_constant]] Push push;
[[vk::binding(0)]] Texture2D<float> glyphAtlas;
[[vk::binding(2)]] SamplerState glyphSampler;

struct Vertex {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 shape : TEXCOORD1;
};
struct Fragment {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation float4 shape : TEXCOORD1;
};

[shader("vertex")]
Fragment vertexMain(Vertex vertex) {
    Fragment result;
    result.position = float4(vertex.position / push.screen.xy * 2.0 - 1.0, 0.0, 1.0);
    result.uv = vertex.uv;
    result.color = vertex.color;
    result.shape = vertex.shape;
    return result;
}

float3 linearColor(float3 color) {
    return select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
}

[shader("fragment")]
float4 fragmentMain(Fragment fragment) : SV_Target {
    float alpha = fragment.color.a;
    if (fragment.shape.x == 1.0) alpha *= glyphAtlas.Sample(glyphSampler, fragment.uv);
    if (fragment.shape.x == 2.0) {
        float2 q = abs(fragment.uv - fragment.shape.zw * 0.5) - fragment.shape.zw * 0.5 + fragment.shape.y;
        float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - fragment.shape.y;
        alpha *= saturate(0.5 - distance);
    }
    return float4(linearColor(fragment.color.rgb) * alpha, alpha);
}
