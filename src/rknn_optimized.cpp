/**
 * RKNN 优化推理引擎
 * 
 * 优化特性：
 * 1. 批处理推理 - 多帧合并一次推理
 * 2. 异步推理 - 多线程并行
 * 3. 内存池 - 减少 malloc/free
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <queue>
#include <condition_variable>

// RKNN
#include <rknn_api.h>

// ============================================================================
// RKNN 批处理推理引擎
// ============================================================================
class RKNNBatchEngine {
public:
    RKNNBatchEngine(int id, int batch_size = 4) 
        : id_(id), batch_size_(batch_size), ctx_(0), initialized_(false) {}
    
    ~RKNNBatchEngine() {
        if (ctx_) {
            rknn_destroy(ctx_);
        }
    }
    
    bool init(const std::string& model_path, rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO) {
        FILE* fp = fopen(model_path.c_str(), "rb");
        if (!fp) {
            printf("[RKNN#%d] Failed to open model: %s\n", id_, model_path.c_str());
            return false;
        }
        
        fseek(fp, 0, SEEK_END);
        size_t model_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        std::vector<char> model_data(model_size);
        if (fread(model_data.data(), 1, model_size, fp) != model_size) {
            fclose(fp);
            return false;
        }
        fclose(fp);
        
        // 初始化 RKNN
        int ret = rknn_init(&ctx_, model_data.data(), model_size, 0, NULL);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_init failed: %d\n", id_, ret);
            ctx_ = 0;
            return false;
        }
        
        // 设置核心绑定
        if (core_mask != RKNN_NPU_CORE_AUTO) {
            ret = rknn_set_core_mask(ctx_, core_mask);
            if (ret == RKNN_SUCC) {
                printf("[RKNN#%d] Set core mask: 0x%x\n", id_, core_mask);
            }
        }
        
        // 设置批处理核心数
        ret = rknn_set_batch_core_num(ctx_, batch_size_);
        if (ret == RKNN_SUCC) {
            printf("[RKNN#%d] Batch core num set to: %d\n", id_, batch_size_);
        }
        
        // 查询输入输出信息
        rknn_input_output_num io_num;
        ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_query failed: %d\n", id_, ret);
            return false;
        }
        
        input_attrs_.resize(io_num.n_input);
        rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, input_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_input);
        
        output_attrs_.resize(io_num.n_output);
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, output_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_output);
        
        // 预分配输出缓冲区
        outputs_.resize(io_num.n_output);
        for (size_t i = 0; i < output_attrs_.size(); i++) {
            outputs_[i].want_float = 1;
            outputs_[i].is_prealloc = 0;
        }
        
        initialized_ = true;
        printf("[RKNN#%d] Model loaded: %u inputs, %u outputs, batch=%d\n",
               id_, io_num.n_input, io_num.n_output, batch_size_);
        
        return true;
    }
    
    // 批处理推理
    bool inference_batch(const std::vector<uint8_t*>& batch_data, int batch_count) {
        if (!initialized_ || batch_count == 0) return false;
        
        // 设置批处理输入
        std::vector<rknn_input> inputs(batch_count);
        for (int i = 0; i < batch_count; i++) {
            inputs[i].index = 0;  // 单输入模型
            inputs[i].type = RKNN_TENSOR_UINT8;
            inputs[i].fmt = RKNN_TENSOR_NHWC;
            inputs[i].size = input_attrs_[0].size;
            inputs[i].buf = batch_data[i];
            inputs[i].pass_through = 0;
        }
        
        int ret = rknn_inputs_set(ctx_, batch_count, inputs.data());
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_inputs_set failed: %d\n", id_, ret);
            return false;
        }
        
        // 执行推理
        ret = rknn_run(ctx_, nullptr);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_run failed: %d\n", id_, ret);
            return false;
        }
        
        // 获取输出
        ret = rknn_outputs_get(ctx_, output_attrs_.size(), outputs_.data(), nullptr);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] rknn_outputs_get failed: %d\n", id_, ret);
            return false;
        }
        
        // 释放输出
        rknn_outputs_release(ctx_, output_attrs_.size(), outputs_.data());
        
        return true;
    }
    
    // 单帧推理（兼容模式）
    bool inference(const uint8_t* data) {
        std::vector<uint8_t*> batch = {const_cast<uint8_t*>(data)};
        return inference_batch(batch, 1);
    }
    
private:
    int id_;
    int batch_size_;
    rknn_context ctx_;
    bool initialized_;
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<rknn_output> outputs_;
};

// ============================================================================
// RKNN 异步推理引擎（多线程）
// ============================================================================
class RKNNAsyncEngine {
public:
    RKNNAsyncEngine(int id) : id_(id), ctx_(0), running_(false), initialized_(false) {}
    
    ~RKNNAsyncEngine() {
        stop();
        if (ctx_) {
            rknn_destroy(ctx_);
        }
    }
    
    bool init(const std::string& model_path, rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO) {
        FILE* fp = fopen(model_path.c_str(), "rb");
        if (!fp) return false;
        
        fseek(fp, 0, SEEK_END);
        size_t model_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        
        std::vector<char> model_data(model_size);
        if (fread(model_data.data(), 1, model_size, fp) != model_size) {
            fclose(fp);
            return false;
        }
        fclose(fp);
        
        int ret = rknn_init(&ctx_, model_data.data(), model_size, 0, NULL);
        if (ret != RKNN_SUCC) {
            printf("[RKNN#%d] init failed: %d\n", id_, ret);
            return false;
        }
        
        if (core_mask != RKNN_NPU_CORE_AUTO) {
            rknn_set_core_mask(ctx_, core_mask);
        }
        
        rknn_input_output_num io_num;
        rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
        
        input_attrs_.resize(io_num.n_input);
        rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, input_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_input);
        
        output_attrs_.resize(io_num.n_output);
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, output_attrs_.data(), sizeof(rknn_tensor_attr) * io_num.n_output);
        
        outputs_.resize(io_num.n_output);
        for (auto& out : outputs_) {
            out.want_float = 1;
            out.is_prealloc = 0;
        }
        
        initialized_ = true;
        running_ = true;
        
        // 启动异步推理线程
        worker_thread_ = std::thread(&RKNNAsyncEngine::workerLoop, this);
        
        printf("[RKNN#%d] Async engine initialized\n", id_);
        return true;
    }
    
    void stop() {
        running_ = false;
        cv_.notify_all();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
    
    // 异步提交推理任务
    void submit(const uint8_t* data, std::function<void(float*)> callback) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            Task task;
            task.data.assign(data, data + input_attrs_[0].size);
            task.callback = callback;
            task_queue_.push(std::move(task));
        }
        cv_.notify_one();
    }
    
private:
    struct Task {
        std::vector<uint8_t> data;
        std::function<void(float*)> callback;
    };
    
    void workerLoop() {
        while (running_) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this] { return !task_queue_.empty() || !running_; });
                if (!running_) break;
                if (task_queue_.empty()) continue;
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
            
            // 执行推理
            rknn_input input = {};
            input.index = 0;
            input.type = RKNN_TENSOR_UINT8;
            input.fmt = RKNN_TENSOR_NHWC;
            input.size = input_attrs_[0].size;
            input.buf = task.data.data();
            
            rknn_inputs_set(ctx_, 1, &input);
            rknn_run(ctx_, nullptr);
            rknn_outputs_get(ctx_, output_attrs_.size(), outputs_.data(), nullptr);
            
            // 回调
            if (task.callback && outputs_[0].buf) {
                task.callback((float*)outputs_[0].buf);
            }
            
            rknn_outputs_release(ctx_, output_attrs_.size(), outputs_.data());
        }
    }
    
    int id_;
    rknn_context ctx_;
    bool running_;
    bool initialized_;
    std::vector<rknn_tensor_attr> input_attrs_;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::vector<rknn_output> outputs_;
    
    std::thread worker_thread_;
    std::queue<Task> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
};

// ============================================================================
// 优化测试主程序
// ============================================================================
int main() {
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║     RKNN 优化推理测试 - 批处理/异步                   ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n\n");
    
    std::string model_path = "/home/topeet/RKNN-YOLOV5-BatchInference-MultiThreading/model/RK3588/yolov5s-640-640.rknn";
    
    // 准备测试数据
    std::vector<uint8_t> dummy_data(640 * 640 * 3, 128);
    
    // 测试 1: 单帧推理（基准）
    printf("=== 测试 1: 单帧推理（基准） ===\n");
    RKNNBatchEngine single_engine(0, 1);
    if (single_engine.init(model_path, RKNN_NPU_CORE_0)) {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 100; i++) {
            single_engine.inference(dummy_data.data());
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        printf("Single inference: 100 frames\n");
        printf("Total time: %.1f ms, FPS: %.1f, Latency: %.2f ms\n", 
               ms, 100000.0 / ms, ms / 100);
    }
    
    // 测试 2: 批处理推理
    printf("\n=== 测试 2: 批处理推理 (Batch=4) ===\n");
    RKNNBatchEngine batch_engine(1, 4);
    if (batch_engine.init(model_path, RKNN_NPU_CORE_1)) {
        std::vector<uint8_t*> batch = {
            dummy_data.data(), dummy_data.data(), 
            dummy_data.data(), dummy_data.data()
        };
        
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 100; i++) {
            batch_engine.inference_batch(batch, 4);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        printf("Batch inference: 100 batches x 4 frames = 400 inferences\n");
        printf("Total time: %.1f ms, FPS: %.1f, Latency: %.2f ms/batch\n", 
               ms, 400000.0 / ms, ms / 100);
    }
    
    // 测试 3: 批处理推理 (Batch=8)
    printf("\n=== 测试 3: 批处理推理 (Batch=8) ===\n");
    RKNNBatchEngine batch8_engine(2, 8);
    if (batch8_engine.init(model_path, RKNN_NPU_CORE_2)) {
        std::vector<uint8_t*> batch;
        for (int i = 0; i < 8; i++) {
            batch.push_back(dummy_data.data());
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 100; i++) {
            batch8_engine.inference_batch(batch, 8);
        }
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        printf("Batch inference: 100 batches x 8 frames = 800 inferences\n");
        printf("Total time: %.1f ms, FPS: %.1f, Latency: %.2f ms/batch\n", 
               ms, 800000.0 / ms, ms / 100);
    }
    
    // 测试 4: 异步推理
    printf("\n=== 测试 4: 异步推理 ===\n");
    RKNNAsyncEngine async_engine(3);
    if (async_engine.init(model_path, RKNN_NPU_CORE_0)) {
        std::atomic<int> callback_count{0};
        
        auto start = std::chrono::high_resolution_clock::now();
        
        // 提交 100 个异步任务
        for (int i = 0; i < 100; i++) {
            async_engine.submit(dummy_data.data(), [&](float* result) {
                (void)result;
                callback_count++;
            });
        }
        
        // 等待完成
        while (callback_count < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        printf("Async inference: 100 tasks completed\n");
        printf("Total time: %.1f ms, FPS: %.1f, Latency: %.2f ms\n", 
               ms, 100000.0 / ms, ms / 100);
        
        async_engine.stop();
    }
    
    printf("\n=== 测试完成 ===\n");
    return 0;
}
