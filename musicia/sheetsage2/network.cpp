module;

#include "network.cuh"
#include <cublasLt.h>
#include <cudnn_frontend.h>

module musicia.sheetsage2.network;

import std;

namespace musicia::sheetsage2 {
    void cuda_error(cudaError_t status) {
        if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
    }

    void blas_error(cublasStatus_t status) {
        if (status != CUBLAS_STATUS_SUCCESS) throw std::runtime_error(cublasGetStatusString(status));
    }

    void dnn_error(cudnnStatus_t status) {
        if (status != CUDNN_STATUS_SUCCESS) throw std::runtime_error(cudnnGetErrorString(status));
    }

    void graph_error(cudnn_frontend::error_t status) {
        if (!status.is_good()) throw std::runtime_error(status.get_message());
    }

    template<typename Function, typename... Arguments>
    void launch(Function kernel, unsigned blocks, unsigned threads, cudaStream_t stream, Arguments... arguments) {
        void* pointers[]{static_cast<void*>(&arguments)...};
        cuda_error(cudaLaunchKernel(reinterpret_cast<const void*>(kernel), dim3(blocks), dim3(threads), pointers, 0, stream));
    }

    Network::Network(const std::filesystem::path& directory) {
        cuda_error(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        blas_error(cublasLtCreate(&blas));
        dnn_error(cudnnCreate(&dnn));
        dnn_error(cudnnSetStream(dnn, stream));
        for (const auto filename : {"mert.safetensors", "sheetsage2.safetensors"}) {
            std::ifstream data(directory / filename, std::ios::binary);
            data.exceptions(std::ios::badbit | std::ios::failbit);
            std::uint64_t header_size{};
            data.read(reinterpret_cast<char*>(&header_size), sizeof(header_size));
            std::string header(header_size, '\0');
            data.read(header.data(), static_cast<std::streamsize>(header_size));
            std::size_t position = 1;
            while ((position = header.find('"', position)) != std::string::npos) {
                const auto end_name = header.find('"', position + 1);
                auto name = header.substr(position + 1, end_name - position - 1);
                const auto end_record = header.find('}', end_name);
                const auto record = header.substr(end_name + 1, end_record - end_name);
                position = end_record + 1;
                if (name == "__metadata__") continue;
                const auto offsets = record.find('[', record.find("data_offsets"));
                const auto offset = std::stoull(record.substr(offsets + 1));
                const auto end = std::stoull(record.substr(record.find(',', offsets) + 1));
                const auto count = static_cast<std::size_t>((end - offset) / sizeof(float));
                if (filename == std::string_view("mert.safetensors")) name = "encoder." + name;
                auto& weight = weights[name];
                weight.count = count;
                std::vector<float> values(count);
                data.seekg(static_cast<std::streamoff>(8 + header_size + offset));
                data.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(float)));
                cuda_error(cudaMalloc(&weight.fp32, count * sizeof(float)));
                cuda_error(cudaMalloc(&weight.bf16, count * sizeof(__nv_bfloat16)));
                cuda_error(cudaMemcpyAsync(weight.fp32, values.data(), count * sizeof(float), cudaMemcpyHostToDevice, stream));
                launch(to_bfloat, static_cast<unsigned>((count + 255) / 256), 256, stream, weight.fp32, weight.bf16, static_cast<int>(count));
                synchronize();
                if (name == "layer_weight") {
                    const float largest = *std::max_element(values.begin(), values.end());
                    float sum = 0;
                    for (int i = 0; i < 25; ++i) sum += layer_weights[i] = std::exp(values[i] - largest);
                    for (auto& coefficient : layer_weights) coefficient /= sum;
                }
            }
        }
        for (auto** pointer : {&hidden, &temporary, &query, &key, &value, &attended, &mixed, &memory}) cuda_error(cudaMalloc(pointer, 7500ull * 1024 * sizeof(float)));
        for (auto** pointer : {&inner, &packed}) cuda_error(cudaMalloc(pointer, 30000ull * 1024 * sizeof(float)));
        cuda_error(cudaMalloc(&audio, 7200000ull * sizeof(float)));
        cuda_error(cudaMalloc(&fft_input, 30000ull * 2048 * sizeof(float)));
        cuda_error(cudaMalloc(&spectrum, 30000ull * 1025 * sizeof(cufftComplex)));
        cuda_error(cudaMalloc(&power, 30000ull * 1025 * sizeof(float)));
        cuda_error(cudaMalloc(&magnitudes, 4096 * sizeof(float)));
        cuda_error(cudaMalloc(&logits, 31678 * sizeof(float)));
        cuda_error(cudaMalloc(&matrix_input, 30000ull * 1024 * sizeof(__nv_bfloat16)));
        for (auto** pointer : {&attention_query, &attention_output, &encoder_key, &encoder_value}) cuda_error(cudaMalloc(pointer, 7500ull * 1024 * sizeof(__nv_bfloat16)));
        for (int i = 0; i < 6; ++i) {
            cuda_error(cudaMalloc(&self_key[i], 5120ull * 512 * sizeof(__nv_bfloat16)));
            cuda_error(cudaMalloc(&self_value[i], 5120ull * 512 * sizeof(__nv_bfloat16)));
            cuda_error(cudaMemsetAsync(self_key[i], 0, 5120ull * 512 * sizeof(__nv_bfloat16), stream));
            cuda_error(cudaMemsetAsync(self_value[i], 0, 5120ull * 512 * sizeof(__nv_bfloat16), stream));
            cuda_error(cudaMalloc(&cross_key[i], 7500ull * 512 * sizeof(__nv_bfloat16)));
            cuda_error(cudaMalloc(&cross_value[i], 7500ull * 512 * sizeof(__nv_bfloat16)));
        }
        cuda_error(cudaMalloc(&matrix_workspace, 32ull * 1024 * 1024));
        cuda_error(cudaMalloc(&tokens, 5120 * sizeof(int)));
        cuda_error(cudaMallocHost(&host_logits, 31678 * sizeof(float)));
        for (int layer = 0; layer < 24; ++layer) {
            for (const auto projection : {"query_proj", "key_proj", "value_proj", "out_proj"}) {
                const auto suffix = "layers." + std::to_string(layer) + ".attn." + projection;
                const auto adapter = "adapter." + suffix;
                auto& weight = weights.at("encoder." + suffix + ".weight");
                matrix(weights.at(adapter + ".lora_B.weight").fp32, adapter + ".lora_A.weight", temporary, 1024, 1024, 64, false, false);
                launch(merge_adapter, 1024 * 1024 / 256, 256, stream, weight.fp32, weight.bf16, temporary, 1024 * 1024);
            }
        }
        synchronize();
        for (auto entry = weights.begin(); entry != weights.end();) {
            if (entry->first.starts_with("adapter.")) {
                cudaFree(entry->second.fp32);
                cudaFree(entry->second.bf16);
                entry = weights.erase(entry);
            } else {
                if (entry->second.count > 4096 && !entry->first.starts_with("encoder.feature_extractor.") && entry->first != "token_embedding.weight" && entry->first != "decoder.embed_positions.weight") cudaFree(std::exchange(entry->second.fp32, nullptr));
                ++entry;
            }
        }
        if (cufftPlan1d(&fft, 2048, CUFFT_R2C, 30000) != CUFFT_SUCCESS) throw std::runtime_error("Unable to create FFT plan.");
        if (cufftSetStream(fft, stream) != CUFFT_SUCCESS) throw std::runtime_error("Unable to set FFT stream.");
    }

    Network::~Network() {
        cudaStreamSynchronize(stream);
        for (auto& [name, weight] : weights) {
            cudaFree(weight.fp32);
            cudaFree(weight.bf16);
        }
        for (auto& [shape, plan] : matrices) {
            cublasLtMatmulDescDestroy(plan.operation);
            cublasLtMatrixLayoutDestroy(plan.a);
            cublasLtMatrixLayoutDestroy(plan.b);
            cublasLtMatrixLayoutDestroy(plan.c);
        }
        for (auto& [shape, plan] : attentions) cudaFree(plan.workspace);
        for (float* pointer : {hidden, temporary, inner, query, key, value, attended, mixed, memory, packed, audio, fft_input, power, magnitudes, logits}) cudaFree(pointer);
        cudaFree(spectrum);
        for (auto* pointer : {matrix_input, attention_query, attention_output, encoder_key, encoder_value}) cudaFree(pointer);
        for (int i = 0; i < 6; ++i) {
            cudaFree(self_key[i]);
            cudaFree(self_value[i]);
            cudaFree(cross_key[i]);
            cudaFree(cross_value[i]);
        }
        cudaFree(tokens);
        cudaFree(matrix_workspace);
        cudaFreeHost(host_logits);
        cufftDestroy(fft);
        cudnnDestroy(dnn);
        cublasLtDestroy(blas);
        cudaStreamDestroy(stream);
    }

    void Network::matrix(const float* input, const std::string& name, float* output, int rows, int columns, int width, bool reduced, bool transpose) {
        const auto shape = std::tuple(rows, columns, width, reduced, transpose);
        auto [entry, created] = matrices.try_emplace(shape);
        auto& plan = entry->second;
        if (created) {
            blas_error(cublasLtMatmulDescCreate(&plan.operation, CUBLAS_COMPUTE_32F, CUDA_R_32F));
            const auto operation = transpose ? CUBLAS_OP_T : CUBLAS_OP_N;
            blas_error(cublasLtMatmulDescSetAttribute(plan.operation, CUBLASLT_MATMUL_DESC_TRANSB, &operation, sizeof(operation)));
            const auto type = reduced ? CUDA_R_16BF : CUDA_R_32F;
            blas_error(cublasLtMatrixLayoutCreate(&plan.a, type, rows, width, width));
            blas_error(cublasLtMatrixLayoutCreate(&plan.b, type, transpose ? columns : width, transpose ? width : columns, transpose ? width : columns));
            blas_error(cublasLtMatrixLayoutCreate(&plan.c, CUDA_R_32F, rows, columns, columns));
            const auto order = CUBLASLT_ORDER_ROW;
            for (auto layout : {plan.a, plan.b, plan.c}) blas_error(cublasLtMatrixLayoutSetAttribute(layout, CUBLASLT_MATRIX_LAYOUT_ORDER, &order, sizeof(order)));
            cublasLtMatmulPreference_t preference{};
            blas_error(cublasLtMatmulPreferenceCreate(&preference));
            const std::size_t size = 32ull * 1024 * 1024;
            blas_error(cublasLtMatmulPreferenceSetAttribute(preference, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, &size, sizeof(size)));
            cublasLtMatmulHeuristicResult_t result{};
            int found{};
            const auto status = cublasLtMatmulAlgoGetHeuristic(blas, plan.operation, plan.a, plan.b, plan.c, plan.c, preference, 1, &result, &found);
            cublasLtMatmulPreferenceDestroy(preference);
            blas_error(status);
            if (!found) throw std::runtime_error("No cuBLASLt algorithm for SheetSage2 matrix.");
            plan.algorithm = result.algo;
        }
        const auto& weight = weights.at(name);
        if (reduced) launch(to_bfloat, (rows * width + 255) / 256, 256, stream, input, matrix_input, rows * width);
        const float alpha = 1.f, beta = 0.f;
        blas_error(cublasLtMatmul(blas, plan.operation, &alpha, reduced ? static_cast<const void*>(matrix_input) : input, plan.a,
            reduced ? static_cast<const void*>(weight.bf16) : weight.fp32, plan.b, &beta, output, plan.c, output, plan.c,
            &plan.algorithm, matrix_workspace, 32ull * 1024 * 1024, stream));
    }

    void Network::linear(const float* input, const std::string& name, float* output, int rows, int width, int columns) {
        matrix(input, name + ".weight", output, rows, columns, width);
        const auto bias = weights.find(name + ".bias");
        launch(bias_round, (rows * columns + 255) / 256, 256, stream, output, bias == weights.end() ? nullptr : bias->second.fp32, columns, rows * columns);
    }

    void Network::norm(const float* input, const std::string& name, float* output, int rows, int width, float epsilon) {
        launch(normalize, rows, 256, stream, input, output, weights.at(name + ".weight").fp32, weights.at(name + ".bias").fp32, width, epsilon);
    }

    void Network::attention(const float* q, __nv_bfloat16* k, __nv_bfloat16* v, float* output, int heads, int q_count, int k_count, bool causal, bool padded) {
        if (q_count == 1) {
            const int length = padded ? cached + 1 : k_count;
            const int segments = (length + 63) / 64;
            launch(decode_attention_partials, heads * segments, 128, stream, q, k, v, packed, heads, length, segments);
            launch(decode_attention_reduce, heads, 128, stream, packed, output, segments);
            return;
        }
        const auto shape = std::tuple(heads, q_count, k_count, causal);
        auto [entry, created] = attentions.try_emplace(shape);
        auto& plan = entry->second;
        if (created) {
            auto& graph = plan.graph;
            graph.set_io_data_type(cudnn_frontend::DataType_t::BFLOAT16).set_intermediate_data_type(cudnn_frontend::DataType_t::FLOAT).set_compute_data_type(cudnn_frontend::DataType_t::FLOAT);
            auto q_tensor = graph.tensor(cudnn_frontend::graph::Tensor_attributes().set_name("Q").set_uid(1).set_dim({1, heads, q_count, 64}).set_stride({q_count * heads * 64, 64, heads * 64, 1}));
            auto k_tensor = graph.tensor(cudnn_frontend::graph::Tensor_attributes().set_name("K").set_uid(2).set_dim({1, heads, k_count, 64}).set_stride({k_count * heads * 64, 64, heads * 64, 1}));
            auto v_tensor = graph.tensor(cudnn_frontend::graph::Tensor_attributes().set_name("V").set_uid(3).set_dim({1, heads, k_count, 64}).set_stride({k_count * heads * 64, 64, heads * 64, 1}));
            auto attributes = cudnn_frontend::graph::SDPA_attributes().set_name("attention").set_generate_stats(false).set_attn_scale(.125f);
            if (causal) attributes.set_diagonal_alignment(cudnn_frontend::DiagonalAlignment_t::TOP_LEFT).set_diagonal_band_right_bound(0);
            auto [result, stats] = graph.sdpa(q_tensor, k_tensor, v_tensor, attributes);
            result->set_output(true).set_uid(4).set_dim({1, heads, q_count, 64}).set_stride({q_count * heads * 64, 64, heads * 64, 1});
            graph_error(graph.validate());
            graph_error(graph.build_operation_graph(dnn));
            graph_error(graph.create_execution_plans({cudnn_frontend::HeurMode_t::A}));
            graph_error(graph.check_support(dnn));
            graph_error(graph.build_plans(dnn, cudnn_frontend::BuildPlanPolicy_t::HEURISTICS_CHOICE));
            cuda_error(cudaMalloc(&plan.workspace, std::max<std::int64_t>(1, graph.get_workspace_size())));
        }
        launch(to_bfloat, (q_count * heads * 64 + 255) / 256, 256, stream, q, attention_query, q_count * heads * 64);
        std::unordered_map<std::int64_t, void*> pointers{{1, attention_query}, {2, k}, {3, v}, {4, attention_output}};
        graph_error(plan.graph.execute(dnn, pointers, plan.workspace));
        launch(from_bfloat, (q_count * heads * 64 + 255) / 256, 256, stream, attention_output, output, q_count * heads * 64);
    }

    void Network::encode(const float* samples, int count) {
        cuda_error(cudaMemsetAsync(audio, 0, 7200000 * sizeof(float), stream));
        cuda_error(cudaMemcpyAsync(audio, samples, count * sizeof(float), cudaMemcpyHostToDevice, stream));
        launch(frames, (30000 * 2048 + 255) / 256, 256, stream, audio, weights.at("encoder.feature_extractor.spectrogram.window").fp32, fft_input, 7200000);
        if (cufftExecR2C(fft, fft_input, spectrum) != CUFFT_SUCCESS) throw std::runtime_error("STFT failed.");
        launch(magnitude_squared, (30000 * 1025 + 255) / 256, 256, stream, spectrum, power, 30000 * 1025);
        matrix(power, "encoder.feature_extractor.mel_scale.fb", hidden, 30000, 128, 1025, false, false);
        launch(log_mel, (30000 * 128 + 255) / 256, 256, stream, hidden, weights.at("encoder.feature_extractor.mel_mean").fp32, weights.at("encoder.feature_extractor.mel_std").fp32);
        int length = 30000, channels = 128;
        for (int stage = 0; stage < 3; ++stage) {
            const auto base = "encoder.subsampling_module." + std::to_string(stage);
            if (stage) {
                norm(hidden, base + ".resampling_layer.0", temporary, length, channels, 1e-6f);
                launch(pack_stride, (length * channels + 255) / 256, 256, stream, temporary, packed, length / 2, channels);
                const int next = stage == 1 ? 512 : 1024;
                linear(packed, base + ".resampling_layer.2", hidden, length / 2, channels * 2, next);
                length /= 2;
                channels = next;
            }
            for (int block = 0; block < 3 + stage; ++block) {
                const auto name = base + ".convnext_layers." + std::to_string(block);
                const int size = length * channels;
                launch(depthwise, (size + 255) / 256, 256, stream, hidden, weights.at(name + ".depthwise_block.1.weight").bf16,
                    weights.at(name + ".depthwise_block.1.bias").fp32, temporary, length, channels, 7);
                norm(temporary, name + ".pointwise_block.0", temporary, length, channels, 1e-6f);
                linear(temporary, name + ".pointwise_block.1", inner, length, channels, channels * 4);
                launch(activate, (size * 4 + 255) / 256, 256, stream, inner, size * 4, true);
                launch(global_magnitude, channels * 4, 256, stream, inner, magnitudes, length, channels * 4);
                launch(global_response, (size * 4 + 255) / 256, 256, stream, inner, magnitudes, weights.at(name + ".pointwise_block.3.weight").fp32,
                    weights.at(name + ".pointwise_block.3.bias").fp32, length, channels * 4);
                linear(inner, name + ".pointwise_block.4", temporary, length, channels * 4, channels);
                launch(residual, (size + 255) / 256, 256, stream, hidden, temporary, size, 1.f, stage != 0);
            }
        }
        const int size = 7500 * 1024;
        launch(mix_layers, (size + 255) / 256, 256, stream, mixed, hidden, size, layer_weights[0], true, true);
        for (int layer = 0; layer < 24; ++layer) {
            const auto name = "encoder.layers." + std::to_string(layer);
            norm(hidden, name + ".ffn1_layer_norm", temporary, 7500, 1024);
            linear(temporary, name + ".ffn1.w_1", inner, 7500, 1024, 4096);
            launch(activate, (size * 4 + 255) / 256, 256, stream, inner, size * 4, true);
            linear(inner, name + ".ffn1.w_2", temporary, 7500, 4096, 1024);
            launch(residual, (size + 255) / 256, 256, stream, hidden, temporary, size, .5f, layer == 0);
            norm(hidden, name + ".attn_layer_norm", temporary, 7500, 1024);
            linear(temporary, name + ".attn.query_proj", query, 7500, 1024, 1024);
            linear(temporary, name + ".attn.key_proj", key, 7500, 1024, 1024);
            linear(temporary, name + ".attn.value_proj", value, 7500, 1024, 1024);
            launch(rotary, (size / 2 + 255) / 256, 256, stream, query, 7500, 1024);
            launch(rotary, (size / 2 + 255) / 256, 256, stream, key, 7500, 1024);
            launch(to_bfloat, (size + 255) / 256, 256, stream, key, encoder_key, size);
            launch(to_bfloat, (size + 255) / 256, 256, stream, value, encoder_value, size);
            attention(query, encoder_key, encoder_value, attended, 16, 7500, 7500);
            linear(attended, name + ".attn.out_proj", temporary, 7500, 1024, 1024);
            launch(residual, (size + 255) / 256, 256, stream, hidden, temporary, size, 1.f, layer == 0);
            norm(hidden, name + ".conv_module.layer_norm", temporary, 7500, 1024);
            linear(temporary, name + ".conv_module.conv_block.1", inner, 7500, 1024, 2048);
            launch(glu, (size + 255) / 256, 256, stream, inner, temporary, size, 1024);
            launch(depthwise, (size + 255) / 256, 256, stream, temporary, weights.at(name + ".conv_module.conv_block.3.weight").bf16, nullptr, attended, 7500, 1024, 31);
            norm(attended, name + ".conv_module.conv_block.4.1", temporary, 7500, 1024);
            launch(activate, (size + 255) / 256, 256, stream, temporary, size, false);
            linear(temporary, name + ".conv_module.conv_block.6", attended, 7500, 1024, 1024);
            launch(residual, (size + 255) / 256, 256, stream, hidden, attended, size, 1.f, layer == 0);
            norm(hidden, name + ".ffn2_layer_norm", temporary, 7500, 1024);
            linear(temporary, name + ".ffn2.w_1", inner, 7500, 1024, 4096);
            launch(activate, (size * 4 + 255) / 256, 256, stream, inner, size * 4, true);
            linear(inner, name + ".ffn2.w_2", temporary, 7500, 4096, 1024);
            launch(residual, (size + 255) / 256, 256, stream, hidden, temporary, size, .5f, layer == 0);
            norm(hidden, name + ".final_layer_norm", hidden, 7500, 1024);
            launch(mix_layers, (size + 255) / 256, 256, stream, mixed, hidden, size, layer_weights[layer + 1], false, false);
        }
        linear(mixed, "encoder_projection", memory, 7500, 1024, 512);
        for (int layer = 0; layer < 6; ++layer) {
            const auto name = "decoder.layers." + std::to_string(layer) + ".encoder_attn";
            linear(memory, name + ".k_proj", key, 7500, 512, 512);
            linear(memory, name + ".v_proj", value, 7500, 512, 512);
            launch(to_bfloat, (7500 * 512 + 255) / 256, 256, stream, key, cross_key[layer], 7500 * 512);
            launch(to_bfloat, (7500 * 512 + 255) / 256, 256, stream, value, cross_value[layer], 7500 * 512);
        }
        cached = 0;
        synchronize();
    }

    void Network::decode(const int* input, int count) {
        cuda_error(cudaMemcpyAsync(tokens, input, count * sizeof(int), cudaMemcpyHostToDevice, stream));
        launch(embed, (count * 512 + 255) / 256, 256, stream, tokens, weights.at("token_embedding.weight").fp32,
            weights.at("decoder.embed_positions.weight").fp32, hidden, count, cached);
        norm(hidden, "decoder.layernorm_embedding", hidden, count, 512);
        const int total = cached + count;
        for (int layer = 0; layer < 6; ++layer) {
            const auto name = "decoder.layers." + std::to_string(layer);
            linear(hidden, name + ".self_attn.q_proj", query, count, 512, 512);
            linear(hidden, name + ".self_attn.k_proj", key, count, 512, 512);
            linear(hidden, name + ".self_attn.v_proj", value, count, 512, 512);
            launch(to_bfloat, (count * 512 + 255) / 256, 256, stream, key, self_key[layer] + cached * 512, count * 512);
            launch(to_bfloat, (count * 512 + 255) / 256, 256, stream, value, self_value[layer] + cached * 512, count * 512);
            attention(query, self_key[layer], self_value[layer], attended, 8, count, cached ? 5120 : count, !cached && count > 1, cached != 0);
            linear(attended, name + ".self_attn.out_proj", temporary, count, 512, 512);
            launch(residual, (count * 512 + 255) / 256, 256, stream, hidden, temporary, count * 512, 1.f, false);
            norm(hidden, name + ".self_attn_layer_norm", hidden, count, 512);
            linear(hidden, name + ".encoder_attn.q_proj", query, count, 512, 512);
            attention(query, cross_key[layer], cross_value[layer], attended, 8, count, 7500);
            linear(attended, name + ".encoder_attn.out_proj", temporary, count, 512, 512);
            launch(residual, (count * 512 + 255) / 256, 256, stream, hidden, temporary, count * 512, 1.f, false);
            norm(hidden, name + ".encoder_attn_layer_norm", hidden, count, 512);
            linear(hidden, name + ".fc1", inner, count, 512, 2048);
            launch(activate, (count * 2048 + 255) / 256, 256, stream, inner, count * 2048, true);
            linear(inner, name + ".fc2", temporary, count, 2048, 512);
            launch(residual, (count * 512 + 255) / 256, 256, stream, hidden, temporary, count * 512, 1.f, false);
            norm(hidden, name + ".final_layer_norm", hidden, count, 512);
        }
        matrix(hidden + (count - 1) * 512, "token_embedding.weight", logits, 1, 31678, 512);
        launch(bias_round, (31678 + 255) / 256, 256, stream, logits, nullptr, 31678, 31678);
        cuda_error(cudaMemcpyAsync(host_logits, logits, 31678 * sizeof(float), cudaMemcpyDeviceToHost, stream));
        cached = total;
        synchronize();
    }

    void Network::synchronize() {
        cuda_error(cudaGetLastError());
        cuda_error(cudaStreamSynchronize(stream));
    }
}
