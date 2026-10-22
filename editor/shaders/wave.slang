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
[[vk::binding(1)]] StructuredBuffer<float> samples;

struct Fragment {
    float4 position : SV_Position;
    float2 edge : TEXCOORD0;
    nointerpolation float2 bounds : TEXCOORD1;
    nointerpolation float energy : TEXCOORD2;
};

[shader("vertex")]
Fragment vertexMain(uint vertex : SV_VulkanVertexID, uint column : SV_VulkanInstanceID) {
    float localFirst = push.view.y + column * push.view.x;
    uint first = min(push.data.x + uint(floor(localFirst)), push.track.z - 1);
    uint last = min(push.data.x + uint(ceil(localFirst + push.view.x)), push.track.z);
    float low = 1e30;
    float high = -1e30;
    float squareSum = 0;
    uint sampleCount = 0;
    uint channelFirst = push.screen.z == 1 ? 0 : push.track.y;
    uint channelEnd = push.screen.z == 1 ? push.track.x : push.track.y + 1;
    for (uint channel = channelFirst; channel < channelEnd; ++channel) {
        if (push.data.z == 0) {
            if (push.view.x < 1.0) {
                float a = samples[push.data.w + first * push.track.x + channel];
                float b = samples[push.data.w + min(first + 1, push.track.z - 1) * push.track.x + channel];
                float value = lerp(a, b, frac(localFirst));
                uint endFrame = min(push.data.x + uint(floor(localFirst + push.view.x)), push.track.z - 1);
                float endA = samples[push.data.w + endFrame * push.track.x + channel];
                float endB = samples[push.data.w + min(endFrame + 1, push.track.z - 1) * push.track.x + channel];
                float next = lerp(endA, endB, frac(localFirst + push.view.x));
                low = min(low, min(value, next));
                high = max(high, max(value, next));
                if (endFrame > first) { low = min(low, b); high = max(high, b); }
            } else {
                for (uint frame = first; frame < max(first + 1, last); ++frame) {
                    float value = samples[push.data.w + frame * push.track.x + channel];
                    low = min(low, value);
                    high = max(high, value);
                    squareSum += value * value;
                    ++sampleCount;
                }
            }
        } else {
            uint begin = first / push.data.z;
            uint end = min((last + push.data.z - 1) / push.data.z, push.track.w);
            for (uint block = begin; block < max(begin + 1, end); ++block) {
                uint offset = push.data.y + (block * push.track.x + channel) * 3;
                low = min(low, samples[offset]);
                high = max(high, samples[offset + 1]);
                uint count = min(last, (block + 1) * push.data.z) - max(first, block * push.data.z);
                squareSum += samples[offset + 2] * count;
                sampleCount += count;
            }
        }
    }
    float center = push.rect.y + push.rect.w * 0.5;
    float amplitude = push.rect.w * 0.4;
    float top = -clamp(high, -1.0, 1.0) * amplitude;
    float bottom = -clamp(low, -1.0, 1.0) * amplitude;
    float rms = sampleCount > 0 ? sqrt(squareSum / sampleCount) * amplitude : 0;
    float radius = (push.screen.z == 1 ? 0.55 : 0.84) * push.screen.w;
    float energy = max(0.4 * push.screen.w, rms);
    float left = -radius - 0.5;
    float right = radius + 0.5;
    float upper = min(top, -energy - radius) - 0.5;
    float lower = max(bottom, energy + radius) + 0.5;
    if (push.view.w != 0) {
        if (bottom - top < 1.0) { top -= 0.5; bottom += 0.5; }
        left = -push.view.z * 0.5;
        right = push.view.z * 0.5;
        upper = top - 0.5;
        lower = bottom + 0.5;
    }
    static const float2 corners[6] = {float2(0,0), float2(1,0), float2(1,1), float2(0,0), float2(1,1), float2(0,1)};
    float2 corner = corners[vertex];
    float2 local = float2(lerp(left, right, corner.x), lerp(upper, lower, corner.y));
    float2 point = float2(push.rect.x + (column + 0.5) * push.view.z, center) + local;
    Fragment result;
    result.position = float4(point / push.screen.xy * 2.0 - 1.0, 0, 1);
    result.edge = local;
    result.bounds = float2(top, bottom);
    result.energy = energy;
    return result;
}

[shader("fragment")]
float4 fragmentMain(Fragment fragment) : SV_Target {
    float peakDistance = max(abs(fragment.edge.x) - 0.4 * push.screen.w,
                             max(fragment.bounds.x - fragment.edge.y, fragment.edge.y - fragment.bounds.y));
    float bodyDistance = length(float2(fragment.edge.x, max(abs(fragment.edge.y) - fragment.energy, 0))) - (push.screen.z == 1 ? 0.55 : 0.84) * push.screen.w;
    float body = saturate(0.5 - bodyDistance);
    // Linear-light blending needs a low opacity to keep the peak stems subdued.
    float peak = saturate(0.5 - peakDistance) * 0.06;
    float alpha = body + peak * (1 - body);
    if (push.view.w != 0) alpha = saturate(min(fragment.edge.y - fragment.bounds.x, fragment.bounds.y - fragment.edge.y) + 0.5) * 0.86;
    float3 color = push.color.rgb;
    if (push.screen.z == 0 && fragment.position.x > push.playhead) color *= 0.72;
    color = select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
    return float4(color * alpha, alpha);
}
