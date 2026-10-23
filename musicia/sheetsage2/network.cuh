#ifndef MUSICIA_SHEETSAGE2_NETWORK_CUH
#define MUSICIA_SHEETSAGE2_NETWORK_CUH

#include <cuda_runtime.h>
#include <cuda_bf16.h>
#include <cufft.h>

namespace musicia::sheetsage2 {
    __global__ void toBfloat(const float* input, __nv_bfloat16* output, int count);
    __global__ void mergeAdapter(float* weight, __nv_bfloat16* reduced, const float* update, int count);
    __global__ void fromBfloat(const __nv_bfloat16* input, float* output, int count);
    __global__ void biasRound(float* output, const float* bias, int width, int count);
    __global__ void normalize(const float* input, float* output, const float* weight, const float* bias, int width, float epsilon);
    __global__ void activate(float* input, int count, bool reduced);
    __global__ void residual(float* input, const float* value, int count, float scale, bool reduced);
    __global__ void mixLayers(float* output, const float* input, int count, float weight, bool reduced, bool first);
    __global__ void depthwise(const float* input, const __nv_bfloat16* weight, const float* bias, float* output, int length, int channels, int kernel);
    __global__ void packStride(const float* input, float* output, int length, int channels);
    __global__ void globalMagnitude(const float* input, float* magnitude, int length, int channels);
    __global__ void globalResponse(float* input, const float* magnitude, const float* weight, const float* bias, int length, int channels);
    __global__ void glu(const float* input, float* output, int count, int width);
    __global__ void rotary(float* input, int length, int width);
    __global__ void decodeAttentionPartials(const float* query, const __nv_bfloat16* key, const __nv_bfloat16* value, float* partials, int heads, int length, int segments);
    __global__ void decodeAttentionReduce(const float* partials, float* output, int segments);
    __global__ void frames(const float* audio, const float* window, float* output, int count);
    __global__ void magnitudeSquared(const cufftComplex* input, float* output, int count);
    __global__ void logMel(float* data, const float* mean, const float* standard);
    __global__ void embed(const int* tokens, const float* embedding, const float* positions, float* output, int count, int offset);
}

#endif
