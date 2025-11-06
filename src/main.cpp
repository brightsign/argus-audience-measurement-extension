#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/core/ocl.hpp>

#include "metrics/file_logger.h"
#include "metrics/log_global.h"
#include "config/configuration.h"
#include "orchestration/orchestrator.h"
#include "input/input_from_registry.h"
#include "input/registry_helper.h"
#include "output/mqtt_broker.h"
#include "util/util.h"

static std::atomic<bool> g_stop{false};
static void on_sig(int){ g_stop.store(true); }

// Install comprehensive crash handlers (SIGSEGV, SIGABRT, terminate, etc)
static void install_crash_handlers() {
  std::set_terminate([](){
    LG_CRIT("FATAL: std::terminate called (uncaught exception).");
    std::fflush(nullptr);
    std::_Exit(134);
  });

  auto sig_handler = [](int s){
    LG_CRIT("FATAL: Signal %d received (segfault/abort)", s);
    std::fflush(nullptr);
    std::_Exit(128 + s);
  };
  
  std::signal(SIGSEGV, sig_handler);
  std::signal(SIGABRT, sig_handler);
  std::signal(SIGFPE,  sig_handler);
  std::signal(SIGILL,  sig_handler);
  std::signal(SIGBUS,  sig_handler);
}

// Crash handler to log uncaught exceptions and other crashes
static void crash_handler() {
    try {
        std::exception_ptr eptr = std::current_exception();
        if (eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& e) {
                LG_CRIT("CRASH: Uncaught exception: %s\n", e.what());
            } catch (...) {
                LG_CRIT("CRASH: Uncaught unknown exception\n");
            }
        } else {
            LG_CRIT("CRASH: std::terminate called (likely std::abort from thread)\n");
        }
    } catch (...) {
        // Last resort, avoid recursive crash
        fprintf(stderr, "CRASH: Double crash in crash handler\n");
    }
    std::fflush(nullptr);
    std::abort();  // Allow core dump if configured
}

struct CliArgs {
  const char* cfg{nullptr};
  const char* model{nullptr};
  const char* input{nullptr};

};

static CliArgs parse_cli(int argc, char** argv) {
  CliArgs a{};
  // flags first
  a.cfg   = get_opt("--config", argc, argv);
  a.model = get_opt("--model",  argc, argv);
  a.input = get_opt("--input",  argc, argv);

  auto is_json  = [](const char* s){ return s && std::string(s).rfind(".json") != std::string::npos; };
  auto is_rknn  = [](const char* s){ return s && std::string(s).rfind(".rknn") != std::string::npos; };
  auto is_flag  = [](const char* s){ return s && s[0]=='-'; };

  // scan positionals (skip flags and their values)
  for (int i=1; i<argc; ++i) {
    if (is_flag(argv[i])) { ++i; continue; }            // skip flag value
    const char* tok = argv[i];
    if (!a.model && is_rknn(tok)) { a.model = tok; continue; }
    if (!a.cfg   && is_json(tok)) { a.cfg   = tok; continue; }
    if (!a.input && !is_flag(tok)) { a.input = tok; continue; }
  }
  return a;
}

int main(int argc, char** argv) {
    // Install crash handlers first, before any other initialization
    install_crash_handlers();
    std::set_terminate(crash_handler);
    
    // OpenCV: keep it deterministic and single-threaded on embedded
    cv::setNumThreads(1);
    #ifdef OPENCV_VERSION
    cv::ocl::setUseOpenCL(false);
    #endif
    
    // ---- logging ----
    FileRotatingLogger::Config logcfg;
    logcfg.path = "/storage/sd/logs/gaze.log";
    logcfg.max_mb = 5;
    logcfg.max_files = 5;
    logcfg.min_level = LogLevel::Debug;
    auto flog = std::make_shared<FileRotatingLogger>(logcfg);
    set_global_logger(flog);
    LG_INFO("Starting attention_demo; log file: %s", flog->path().c_str());

    // Parse CLI once
    CliArgs cli = parse_cli(argc, argv);

    // Pick config path (CLI or default search)
    std::string cfg_path;
    if (cli.cfg && file_exists(cli.cfg)) {
    cfg_path = cli.cfg;
    } else {
    cfg_path = pick_config_path(argc, argv);
    }
    LG_INFO("Config path selected: %s", cfg_path.c_str());

    // Load config
    AppConfig appcfg{};
    char err[256]{};
    if (!config::load_from_file(cfg_path, appcfg, /*strict=*/false, err, sizeof(err))) {
    LG_WARN("Config load failed from %s: %s (continuing with defaults)", cfg_path.c_str(), err);
    }

    // Override model path if CLI provided
    if (cli.model && file_exists(cli.model)) {
        LG_INFO("CLI: overriding primary model -> %s", cli.model);
        appcfg.primary_model.model_path = cli.model;
    }
    // Build input from CLI (if present) else registry
    InputConfig effective_input = appcfg.input;
    if (cli.input) {
        LG_INFO("CLI: overriding input -> %s", cli.input);
        effective_input = make_input_from_registry_value(cli.input); // same mapper works for CLI
    } else {
        // Registry fallback
        const std::string choice = RegistryHelper::getVideoDevice();
        LG_INFO("Registry: video-device='%s'", choice.c_str());
        InputConfig reg_input = make_input_from_registry_value(choice);
        if (!reg_input.rtsp_url.empty() || !reg_input.usb_device.empty() || !reg_input.file_path.empty()) {
            effective_input = reg_input;
        }
   }
   // Validate after overrides
    if (!appcfg.validate(err, sizeof(err))) {
        LG_ERROR("Config invalid: %s", err);
        return 1;
    }

    PipelineConfig pc{};
    pc.input = effective_input;
    pc.heartbeat_timeout_ms = appcfg.runtime.heartbeat_ms;
    
    // Set primary model (RetinaFace for face/gaze detection)
    pc.primary_model = appcfg.primary_model;
    pc.primary_model.npu_core = appcfg.primary_model.npu_core;  // From config
    
    // TODO: Set secondary model (YOLOX for object/person detection)
    // Load from config if available, otherwise use defaults
    if (!appcfg.secondary_models.empty()) {
      const auto& cfg = appcfg.secondary_models[0];  // Use first secondary model from config
      pc.secondary_model.name = cfg.name;
      pc.secondary_model.model_path = cfg.model_path;
      pc.secondary_model.family = ModelFamily::YOLOX;
      pc.secondary_model.backend = Backend::RKNN;
      pc.secondary_model.task = TaskType::Detector;
      pc.secondary_model.input_size = {cfg.input_size.w, cfg.input_size.h};
      pc.secondary_model.input_channels = 3;
      pc.secondary_model.input_layout = ColorLayout::BGR;
      pc.secondary_model.order = ChannelOrder::HWC;
      pc.secondary_model.conf_threshold = cfg.conf_threshold;
      pc.secondary_model.nms_threshold = cfg.nms_threshold;
      pc.secondary_model.npu_core = cfg.npu_core >= 0 ? cfg.npu_core : 1;  // From config or default to core 1
    } else {
      // Fallback defaults if no secondary model in config
      pc.secondary_model.name = "yolox-s";
      pc.secondary_model.model_path = "model/yolox_s.rknn";
      pc.secondary_model.family = ModelFamily::YOLOX;
      pc.secondary_model.backend = Backend::RKNN;
      pc.secondary_model.task = TaskType::Detector;
      pc.secondary_model.input_size = {640, 640};
      pc.secondary_model.input_channels = 3;
      pc.secondary_model.input_layout = ColorLayout::BGR;
      pc.secondary_model.order = ChannelOrder::HWC;
      pc.secondary_model.conf_threshold = 0.5f;
      pc.secondary_model.nms_threshold = 0.45f;
      pc.secondary_model.npu_core = 1;  // Default to core 1
    }

    // Apply test/debug mode settings from config
    // Default: both enabled
    // If test_face_only=true: enable only face, disable yolo
    // If test_yolo_only=true: enable only yolo, disable face
    if (appcfg.test_face_only && appcfg.test_yolo_only) {
        LG_WARN("Config error: both test_face_only and test_yolo_only are true! Enabling both models.");
        pc.enable_face_model = true;
        pc.enable_yolo_model = true;
    } else if (appcfg.test_face_only) {
        pc.enable_face_model = true;
        pc.enable_yolo_model = false;
    } else if (appcfg.test_yolo_only) {
        pc.enable_face_model = false;
        pc.enable_yolo_model = true;
    } else {
        // Default: both enabled
        pc.enable_face_model = true;
        pc.enable_yolo_model = true;
    }

    LG_INFO("Test mode config: test_face_only=%s test_yolo_only=%s",
            appcfg.test_face_only ? "true" : "false",
            appcfg.test_yolo_only ? "true" : "false");

    LG_INFO("Inference pipeline: enable_face_model=%s enable_yolo_model=%s",
            pc.enable_face_model ? "true" : "false",
            pc.enable_yolo_model ? "true" : "false");

    // Configure frame output (optional)
    pc.enable_frame_output = appcfg.enable_frame_output;
    pc.output_dir = appcfg.output_dir;
    pc.max_frames = appcfg.max_frames;
    pc.frame_quality = appcfg.frame_quality;
    
    // If frame output not configured, enable with defaults
    if (!pc.enable_frame_output) {
        pc.enable_frame_output = true;
        pc.output_dir = "/tmp";
        pc.max_frames = 1;        // Keep only latest frame (overwrite)
        pc.frame_quality = 85;
        LG_INFO("Frame output not configured, enabling defaults: dir=/tmp max_frames=1 quality=85");
    }
    
    if (pc.enable_frame_output && !pc.output_dir.empty()) {
        LG_INFO("Frame output enabled: dir=%s max_frames=%d quality=%d",
                pc.output_dir.c_str(), pc.max_frames, pc.frame_quality);
    }

    LG_INFO("Selected input: usb=%s rtsp=%s file=%s",
            pc.input.usb_device.c_str(),
            pc.input.rtsp_url.c_str(),
            pc.input.file_path.c_str());
   
    // ---- Start embedded MQTT broker ----
    MqttBroker::Cfg broker_cfg;
    broker_cfg.port = 1883;
    broker_cfg.bind_address = "0.0.0.0";
    broker_cfg.allow_anonymous = true;
    MqttBroker broker{broker_cfg};
    if (!broker.start()) {
        LG_WARN("Failed to start embedded MQTT broker (continuing without it)");
    }

    // ---- run ----
    Orchestrator orch{pc};
    if (!orch.start()) {
        LG_ERROR("Failed to start orchestrator");
        return 1;
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    orch.request_stop();
    orch.join();
    LG_INFO("Shutdown complete");
    return 0;
}
