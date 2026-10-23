module;

#include <cuda_runtime.h>
#include <cuda_bf16.h>
#include <cublasLt.h>
#include <cufft.h>
#include <cudnn_frontend.h>

export module musicia.sheetsage2.network;

import std;

export namespace musicia::sheetsage2 {
    struct Weight {
        float* fp32{};
        __nv_bfloat16* bf16{};
        std::size_t count{};
    };

    struct MatrixPlan {
        cublasLtMatmulDesc_t operation{};
        cublasLtMatrixLayout_t a{}, b{}, c{};
        cublasLtMatmulAlgo_t algorithm{};
    };

    struct AttentionPlan {
        cudnn_frontend::graph::Graph graph;
        void* workspace{};
    };

    struct Network {
        cudaStream_t stream{};
        cublasLtHandle_t blas{};
        cudnnHandle_t dnn{};
        cufftHandle fft{};
        std::map<std::string, Weight> weights;
        std::map<std::tuple<int, int, int, bool, bool>, MatrixPlan> matrices;
        std::map<std::tuple<int, int, int, bool>, AttentionPlan> attentions;
        float *hidden{}, *temporary{}, *inner{}, *query{}, *key{}, *value{}, *attended{}, *mixed{}, *memory{}, *packed{}, *audio{}, *fftInput{}, *power{}, *magnitudes{}, *logits{};
        cufftComplex* spectrum{};
        __nv_bfloat16 *matrixInput{}, *attentionQuery{}, *attentionOutput{};
        std::array<__nv_bfloat16*, 6> selfKey{}, selfValue{}, crossKey{}, crossValue{};
        __nv_bfloat16 *encoderKey{}, *encoderValue{};
        void* matrixWorkspace{};
        int* tokens{};
        float* hostLogits{};
        int cached{};
        std::array<float, 25> layerWeights{};

        explicit Network(const std::filesystem::path& directory);
        ~Network();
        Network(const Network&) = delete;
        Network& operator=(const Network&) = delete;
        void matrix(const float* input, const std::string& weight, float* output, int rows, int columns, int width, bool reduced = true, bool transpose = true);
        void linear(const float* input, const std::string& name, float* output, int rows, int width, int columns);
        void norm(const float* input, const std::string& name, float* output, int rows, int width, float epsilon = 1e-5f);
        void attention(const float* q, __nv_bfloat16* k, __nv_bfloat16* v, float* output, int heads, int qCount, int kCount, bool causal = false, bool padded = false);
        void encode(const float* samples, int count);
        void decode(const int* input, int count);
        void synchronize();
    };
}

