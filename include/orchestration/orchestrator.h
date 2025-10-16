#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include <atomic>
#include <thread>
#include <memory>

#include "model_spec.h"
#include "input_source.h"
#include "preprocessor.h"
#include "model_runner.h"
#include "postprocessor.h"
#include "frame_queue.h"
#include "queue.h"       // ThreadSafeQueue<InferenceResult>

class Orchestrator {
public:
    Orchestrator(std::unique_ptr<IInputSource> source,
                 std::unique_ptr<IPreprocessor> preproc,
                 std::unique_ptr<IModelRunner> model,
                 std::unique_ptr<IPostProcessor> post,
                 ThreadSafeQueue<InferenceResult>& resultQ,
                 std::atomic<bool>& running,
                 int target_fps = 20);

    ~Orchestrator();

    // Start capture + pipeline threads
    void start();

    // Stop and join threads
    void stop();

private:
    void captureThreadFn();
    void preprocessThreadFn();
    void inferenceThreadFn();

private:
    std::unique_ptr<IInputSource> source_;
    std::unique_ptr<IPreprocessor> preproc_;
    std::unique_ptr<IModelRunner>  model_;
    std::unique_ptr<IPostProcessor> post_;

    ThreadSafeQueue<InferenceResult>& resultQ_;
    std::atomic<bool>& running_;
    int target_fps_{20};

    // Queues between stages
    OverwriteQueue<CaptureFrame>  q_cap_{2};
    OverwriteQueue<Preprocessed>  q_pre_{1};

    // Threads
    std::thread t_cap_;
    std::thread t_pre_;
    std::thread t_inf_;

    // Stats
    std::atomic<long long> cap_ns_{0};
    std::atomic<long long> pre_ns_{0};
    std::atomic<long long> inf_ns_{0};
};

#endif // ORCHESTRATOR_H

