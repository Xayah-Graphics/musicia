struct Push {
    float4 rect;
    float4 view;
    uint4 data;
    uint4 track;
    float4 screen;
    float4 color;
    float focus_strength;
};
[[vk::push_constant]] Push push;
[[vk::binding(0)]] Texture2D<float> glyph_atlas;
[[vk::binding(2)]] SamplerState glyph_sampler;
[[vk::binding(3)]] StructuredBuffer<float2> focus_columns;

struct Vertex {
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float4 shape : TEXCOORD1;
    float2 focus_levels : TEXCOORD2;
};
struct Fragment {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation float4 shape : TEXCOORD1;
    nointerpolation float2 focus_levels : TEXCOORD2;
};

[shader("vertex")]
Fragment vertex_main(Vertex vertex) {
    Fragment result;
    result.position = float4(vertex.position / push.screen.xy * 2.0 - 1.0, 0.0, 1.0);
    result.uv = vertex.uv;
    result.color = vertex.color;
    result.shape = vertex.shape;
    result.focus_levels = vertex.focus_levels;
    return result;
}

float3 linear_color(float3 color) {
    return select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
}

[shader("fragment")]
float4 fragment_main(Fragment fragment) : SV_Target {
    float alpha = fragment.color.a;
    if (fragment.shape.x == 1.0) alpha *= glyph_atlas.Sample(glyph_sampler, fragment.uv);
    if (fragment.shape.x == 2.0) {
        float2 q = abs(fragment.uv - fragment.shape.zw * 0.5) - fragment.shape.zw * 0.5 + fragment.shape.y;
        float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - fragment.shape.y;
        alpha *= saturate(0.5 - distance);
    }
    float brightness = 1;
    if (push.focus_strength > 0 && fragment.focus_levels.x < 1) {
        float2 emphasis = focus_columns[uint(fragment.position.x)];
        brightness -= (1 - fragment.focus_levels.x) * (push.focus_strength - emphasis.x);
        brightness += (fragment.focus_levels.y - fragment.focus_levels.x) * emphasis.y;
    }
    return float4(linear_color(fragment.color.rgb * brightness) * alpha, alpha);
}
