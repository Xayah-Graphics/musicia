#include "network.cuh"
#include <math_constants.h>

namespace musicia::sheetsage2 {
    __device__ float rounded(float value) {
        return __bfloat162float(__float2bfloat16_rn(value));
    }

    __global__ void toBfloat(const float* input, __nv_bfloat16* output, int count) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) output[i] = __float2bfloat16_rn(input[i]);
    }

    __global__ void mergeAdapter(float* weight, __nv_bfloat16* reduced, const float* update, int count) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) {
            weight[i] += update[i] * 2.f;
            reduced[i] = __float2bfloat16_rn(weight[i]);
        }
    }

    __global__ void fromBfloat(const __nv_bfloat16* input, float* output, int count) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) output[i] = __bfloat162float(input[i]);
    }

    __global__ void biasRound(float* output, const float* bias, int width, int count) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) output[i] = rounded(output[i] + (bias ? rounded(bias[i % width]) : 0.f));
    }

    __global__ void normalize(const float* input, float* output, const float* weight, const float* bias, int width, float epsilon) {
        __shared__ float sums[256];
        __shared__ float mean, variance;
        const int row = blockIdx.x;
        float sum = 0;
        for (int i = threadIdx.x; i < width; i += blockDim.x) sum += input[row * width + i];
        sums[threadIdx.x] = sum;
        __syncthreads();
        for (int stride = blockDim.x / 2; stride; stride /= 2) {
            if (threadIdx.x < stride) sums[threadIdx.x] += sums[threadIdx.x + stride];
            __syncthreads();
        }
        if (threadIdx.x == 0) mean = sums[0] / width;
        __syncthreads();
        sum = 0;
        for (int i = threadIdx.x; i < width; i += blockDim.x) {
            const float x = input[row * width + i] - mean;
            sum += x * x;
        }
        sums[threadIdx.x] = sum;
        __syncthreads();
        for (int stride = blockDim.x / 2; stride; stride /= 2) {
            if (threadIdx.x < stride) sums[threadIdx.x] += sums[threadIdx.x + stride];
            __syncthreads();
        }
        if (threadIdx.x == 0) variance = rsqrtf(sums[0] / width + epsilon);
        __syncthreads();
        for (int i = threadIdx.x; i < width; i += blockDim.x) output[row * width + i] = (input[row * width + i] - mean) * variance * weight[i] + bias[i];
    }

    __global__ void activate(float* input, int count, bool reduced) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) {
            const float x = input[i];
            const float y = .5f * x * (1.f + erff(x * .7071067811865475244f));
            input[i] = reduced ? rounded(y) : y;
        }
    }

    __global__ void residual(float* input, const float* value, int count, float scale, bool reduced) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) {
            const float y = input[i] + rounded(value[i] * scale);
            input[i] = reduced ? rounded(y) : y;
        }
    }

    __global__ void mixLayers(float* output, const float* input, int count, float weight, bool reduced, bool first) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) {
            float y = input[i] * weight;
            if (reduced) y = rounded(y);
            output[i] = first ? y : output[i] + y;
        }
    }

    __global__ void depthwise(const float* input, const __nv_bfloat16* weight, const float* bias, float* output, int length, int channels, int kernel) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i >= length * channels) return;
        const int t = i / channels, channel = i % channels;
        float sum = 0;
        for (int j = 0; j < kernel; ++j) {
            const int source = t + j - kernel / 2;
            if (source >= 0 && source < length) sum += rounded(input[source * channels + channel]) * __bfloat162float(weight[channel * kernel + j]);
        }
        output[i] = rounded(sum + (bias ? rounded(bias[channel]) : 0.f));
    }

    __global__ void packStride(const float* input, float* output, int length, int channels) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < length * channels * 2) {
            const int row = i / (channels * 2), column = i % (channels * 2);
            output[i] = input[(row * 2 + column % 2) * channels + column / 2];
        }
    }

    __global__ void globalMagnitude(const float* input, float* magnitude, int length, int channels) {
        __shared__ float sums[256];
        const int c = blockIdx.x;
        float sum = 0;
        for (int t = threadIdx.x; t < length; t += blockDim.x) {
            const float x = input[t * channels + c];
            sum += x * x;
        }
        sums[threadIdx.x] = sum;
        __syncthreads();
        for (int stride = blockDim.x / 2; stride; stride /= 2) {
            if (threadIdx.x < stride) sums[threadIdx.x] += sums[threadIdx.x + stride];
            __syncthreads();
        }
        if (!threadIdx.x) magnitude[c] = sqrtf(sums[0]);
    }

    __global__ void globalResponse(float* input, const float* magnitude, const float* weight, const float* bias, int length, int channels) {
        __shared__ float denominator;
        __shared__ float sums[256];
        float sum = 0;
        for (int c = threadIdx.x; c < channels; c += blockDim.x) sum += magnitude[c];
        sums[threadIdx.x] = sum;
        __syncthreads();
        for (int stride = blockDim.x / 2; stride; stride /= 2) {
            if (threadIdx.x < stride) sums[threadIdx.x] += sums[threadIdx.x + stride];
            __syncthreads();
        }
        if (!threadIdx.x) denominator = sums[0] / channels + 1e-6f;
        __syncthreads();
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < length * channels) {
            const int c = i % channels;
            const float x = input[i];
            input[i] = weight[c] * (x * (magnitude[c] / denominator)) + bias[c] + x;
        }
    }

    __global__ void glu(const float* input, float* output, int count, int width) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) {
            const int row = i / width, column = i % width;
            output[i] = rounded(input[row * width * 2 + column] / (1.f + expf(-input[row * width * 2 + width + column])));
        }
    }

    __global__ void rotary(float* input, int length, int width) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < length * width / 2) {
            const int t = i / (width / 2), h = i % (width / 2) / 32, d = i % 32;
            const int offset = t * width + h * 64 + d;
            const float frequency = rounded(1.f / powf(10000.f, static_cast<float>(d) / 32.f));
            const float angle = rounded(rounded(static_cast<float>(t)) * frequency);
            const float c = rounded(cosf(angle)), s = rounded(sinf(angle));
            const float a = input[offset], b = input[offset + 32];
            input[offset] = rounded(rounded(a * c) + rounded(-b * s));
            input[offset + 32] = rounded(rounded(b * c) + rounded(a * s));
        }
    }

    __global__ void decodeAttentionPartials(const float* query, const __nv_bfloat16* key, const __nv_bfloat16* value, float* partials, int heads, int length, int segments) {
        __shared__ float maxima[4], sums[4], values[4][64];
        const int head = blockIdx.x / segments, segment = blockIdx.x % segments;
        const int warp = threadIdx.x / 32, lane = threadIdx.x % 32;
        const float q0 = query[head * 64 + lane], q1 = query[head * 64 + lane + 32];
        float maximum = -CUDART_INF_F, sum = 0, v0 = 0, v1 = 0;
        for (int token = segment * 64 + warp; token < min(length, (segment + 1) * 64); token += 4) {
            const int offset = (token * heads + head) * 64 + lane;
            float score = q0 * __bfloat162float(key[offset]) + q1 * __bfloat162float(key[offset + 32]);
            for (int shift = 16; shift; shift /= 2) score += __shfl_down_sync(0xffffffff, score, shift);
            score = __shfl_sync(0xffffffff, score, 0) * .125f;
            const float next = fmaxf(maximum, score);
            const float previous = expf(maximum - next), probability = expf(score - next);
            v0 = v0 * previous + probability * __bfloat162float(value[offset]);
            v1 = v1 * previous + probability * __bfloat162float(value[offset + 32]);
            sum = sum * previous + probability;
            maximum = next;
        }
        if (!lane) {
            maxima[warp] = maximum;
            sums[warp] = sum;
        }
        values[warp][lane] = v0;
        values[warp][lane + 32] = v1;
        __syncthreads();
        if (!warp) {
            maximum = fmaxf(fmaxf(maxima[0], maxima[1]), fmaxf(maxima[2], maxima[3]));
            sum = v0 = v1 = 0;
            for (int part = 0; part < 4; ++part) {
                const float scale = sums[part] > 0 ? expf(maxima[part] - maximum) : 0.f;
                sum += sums[part] * scale;
                v0 += values[part][lane] * scale;
                v1 += values[part][lane + 32] * scale;
            }
            const int offset = blockIdx.x * 66;
            if (!lane) {
                partials[offset] = maximum;
                partials[offset + 1] = sum;
            }
            partials[offset + 2 + lane] = v0;
            partials[offset + 34 + lane] = v1;
        }
    }

    __global__ void decodeAttentionReduce(const float* partials, float* output, int segments) {
        __shared__ float scales[128], sums[128], maximum, denominator;
        const int thread = threadIdx.x, head = blockIdx.x;
        const int offset = (head * segments + thread) * 66;
        sums[thread] = thread < segments ? partials[offset] : -CUDART_INF_F;
        __syncthreads();
        for (int stride = 64; stride; stride /= 2) {
            if (thread < stride) sums[thread] = fmaxf(sums[thread], sums[thread + stride]);
            __syncthreads();
        }
        if (!thread) maximum = sums[0];
        __syncthreads();
        scales[thread] = thread < segments ? expf(partials[offset] - maximum) : 0.f;
        sums[thread] = thread < segments ? scales[thread] * partials[offset + 1] : 0.f;
        __syncthreads();
        for (int stride = 64; stride; stride /= 2) {
            if (thread < stride) sums[thread] += sums[thread + stride];
            __syncthreads();
        }
        if (!thread) denominator = sums[0];
        __syncthreads();
        if (thread < 64) {
            float sum = 0;
            for (int part = 0; part < segments; ++part) sum += scales[part] * partials[(head * segments + part) * 66 + 2 + thread];
            output[head * 64 + thread] = rounded(sum / denominator);
        }
    }

    __global__ void frames(const float* audio, const float* window, float* output, int count) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < 30000 * 2048) {
            int sample = i / 2048 * 240 + i % 2048 - 1024;
            if (sample < 0) sample = -sample;
            if (sample >= count) sample = 2 * count - 2 - sample;
            output[i] = audio[sample] * window[i % 2048];
        }
    }

    __global__ void magnitudeSquared(const cufftComplex* input, float* output, int count) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count) output[i] = input[i].x * input[i].x + input[i].y * input[i].y;
    }

    __global__ void logMel(float* data, const float* mean, const float* standard) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < 30000 * 128) data[i] = (10.f * log10f(fmaxf(data[i], 1e-10f)) - mean[i % 128]) / fmaxf(standard[i % 128], 1e-5f);
    }

    __global__ void embed(const int* tokens, const float* embedding, const float* positions, float* output, int count, int offset) {
        const int i = blockIdx.x * blockDim.x + threadIdx.x;
        if (i < count * 512) output[i] = embedding[tokens[i / 512] * 512 + i % 512] + positions[(offset + i / 512 + 2) * 512 + i % 512];
    }

}
