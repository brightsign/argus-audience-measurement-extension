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
#include "output/face_blur.h"
#include "util/util.h"
#ifdef DEMO_MODE_ENABLED
#include "demo/demo_license_checker.h"
#endif

static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_restart_requested{false};
static void on_sig(int){ g_stop.store(true); }

// Test function to verify atomic flag operations
static void test_atomic_flags() {
    LG_INFO("========================================");
    LG_INFO("TESTING ATOMIC FLAG OPERATIONS");
    LG_INFO("========================================");
    LG_INFO("TEST: Initial states - g_stop=%d g_restart_requested=%d", 
            g_stop.load(), g_restart_requested.load());
    
    // Test setting restart flag
    LG_INFO("TEST: Setting g_restart_requested to true...");
    g_restart_requested.store(true);
    LG_INFO("TEST: After store - g_restart_requested=%d", g_restart_requested.load());
    
    // Test resetting
    LG_INFO("TEST: Resetting g_restart_requested to false...");
    g_restart_requested.store(false);
    LG_INFO("TEST: After reset - g_restart_requested=%d", g_restart_requested.load());
    
    LG_INFO("TEST: Atomic flag test PASSED");
    LG_INFO("========================================");
}

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
  const char* license_file{nullptr};
};

static CliArgs parse_cli(int argc, char** argv) {
  CliArgs a{};
  // flags first
  a.cfg   = get_opt("--config", argc, argv);
  a.model = get_opt("--model",  argc, argv);
  a.input = get_opt("--input",  argc, argv);
  a.license_file = get_opt("--license-file", argc, argv);

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
    
    // ---- logging (initial setup - use /tmp to avoid boot-time SD mount race) ----
    FileRotatingLogger::Config logcfg;
    logcfg.path = "/tmp/gaze.log";
    logcfg.max_mb = 5;
    logcfg.max_files = 5;
    logcfg.min_level = LogLevel::Info;  // Start with Info, will be updated from config
    auto flog = std::make_shared<FileRotatingLogger>(logcfg);
    set_global_logger(flog);
    LG_INFO("Starting attention_demo; initial log file: %s", flog->path().c_str());
    
    // TEST: Verify atomic flag operations work correctly
    test_atomic_flags();

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

    // Apply log level from config
    {
      LogLevel level = LogLevel::Info;  // default to Info
      std::string log_level_str = appcfg.log_level;
      // Normalize to lowercase
      for (auto& c : log_level_str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      
      if (log_level_str == "debug") level = LogLevel::Debug;
      else if (log_level_str == "info") level = LogLevel::Info;
      else if (log_level_str == "warn" || log_level_str == "warning") level = LogLevel::Warn;
      else if (log_level_str == "error") level = LogLevel::Error;
      else if (log_level_str == "critical") level = LogLevel::Critical;
      else {
        LG_WARN("Invalid log_level '%s' in config, defaulting to 'info'", appcfg.log_level.c_str());
      }
      
      flog->set_level(level);
      const char* level_names[] = {"debug", "info", "warn", "error", "critical"};
      LG_INFO("Log level set to: %s", level_names[static_cast<int>(level)]);
    }

    // Re-initialize logger to configured log_dir if specified
    if (!appcfg.log_dir.empty()) {
      std::string log_path = appcfg.log_dir + "/gaze.log";
      FileRotatingLogger::Config cfg2;
      cfg2.path      = log_path;
      cfg2.max_mb    = 5;
      cfg2.max_files = 5;
      cfg2.min_level = flog->level();
      auto flog2 = std::make_shared<FileRotatingLogger>(cfg2);
      if (flog2->is_open()) {
        set_global_logger(flog2);
        flog = flog2;
        LG_INFO("Logger switched to: %s", log_path.c_str());
      } else {
        LG_WARN("Could not open log at %s, keeping /tmp/gaze.log", log_path.c_str());
      }
    }

    // Override model path if CLI provided
    if (cli.model && file_exists(cli.model)) {
        LG_INFO("CLI: overriding primary model -> %s", cli.model);
        appcfg.primary_model.model_path = cli.model;
    }
    // Build input with configurable priority based on input_source_priority setting
    // Priority is always: CLI args > [config or registry] > [other] > auto-detect
    InputConfig effective_input = appcfg.input;
    
    // Normalize priority value (default to "config" if invalid)
    std::string priority = appcfg.input_source_priority;
    if (priority != "config" && priority != "registry") {
        LG_WARN("Invalid input_source_priority '%s', defaulting to 'config'", priority.c_str());
        priority = "config";
    }
    
    if (cli.input) {
        // Priority 1: CLI argument (always highest priority for manual override)
        LG_INFO("CLI: overriding input -> %s", cli.input);
        effective_input = make_input_from_registry_value(cli.input);
    } else if (priority == "config") {
        // Priority setting: "config" - use argus-config.json input based on input_source selection
        const std::string& input_src = appcfg.input_source;
        LG_INFO("Config: using input from argus-config.json (priority='config', input_source='%s')", input_src.c_str());
        
        // Select input based on input_source field
        if (input_src == "rtsp" && !appcfg.input.rtsp_url.empty()) {
            LG_INFO("  - Selected RTSP URL: %s", appcfg.input.rtsp_url.c_str());
            effective_input.rtsp_url = appcfg.input.rtsp_url;
            effective_input.rtsp = appcfg.input.rtsp;
            // Clear other inputs to avoid conflicts
            effective_input.usb_device.clear();
            effective_input.file_path.clear();
        } else if (input_src == "usb") {
            std::string detected_usb = autoDetectUsbDeviceV4L2();
            if (detected_usb.empty()) detected_usb = "/dev/video0";
            LG_INFO("  - Auto-detected USB device: %s", detected_usb.c_str());
            effective_input.usb_device = detected_usb;
            effective_input.usb = appcfg.input.usb;
            // Clear other inputs to avoid conflicts
            effective_input.rtsp_url.clear();
            effective_input.file_path.clear();
        } else if (input_src == "file" && !appcfg.input.file_path.empty()) {
            LG_INFO("  - Selected File path: %s", appcfg.input.file_path.c_str());
            effective_input.file_path = appcfg.input.file_path;
            effective_input.file = appcfg.input.file;
            // Clear other inputs to avoid conflicts
            effective_input.rtsp_url.clear();
            effective_input.usb_device.clear();
        } else {
            // Selected input not configured, fallback to first available
            LG_WARN("Selected input_source='%s' not configured, falling back to first available", input_src.c_str());
            if (!appcfg.input.rtsp_url.empty()) {
                LG_INFO("  - Fallback to RTSP URL: %s", appcfg.input.rtsp_url.c_str());
                effective_input.rtsp_url = appcfg.input.rtsp_url;
                effective_input.rtsp = appcfg.input.rtsp;
            } else if (!appcfg.input.usb_device.empty() || input_src == "usb") {
                std::string detected_usb = autoDetectUsbDeviceV4L2();
                if (detected_usb.empty()) detected_usb = "/dev/video0";
                LG_INFO("  - Fallback to auto-detected USB device: %s", detected_usb.c_str());
                effective_input.usb_device = detected_usb;
                effective_input.usb = appcfg.input.usb;
            } else if (!appcfg.input.file_path.empty()) {
                LG_INFO("  - Fallback to File path: %s", appcfg.input.file_path.c_str());
                effective_input.file_path = appcfg.input.file_path;
                effective_input.file = appcfg.input.file;
            } else {
                // All inputs empty, try registry fallback
                const std::string choice = RegistryHelper::getVideoDevice();
                LG_INFO("Config empty, using registry fallback: video-device='%s'", choice.c_str());
                InputConfig reg_input = make_input_from_registry_value(choice);
                if (!reg_input.rtsp_url.empty() || !reg_input.usb_device.empty() || !reg_input.file_path.empty()) {
                    effective_input = reg_input;
                } else {
                    LG_INFO("No input configured, using auto-detection");
                }
            }
        }
    } else {
        // Priority setting: "registry" - prefer registry over argus-config.json
        const std::string choice = RegistryHelper::getVideoDevice();
        LG_INFO("Registry: video-device='%s' (priority='registry')", choice.c_str());
        InputConfig reg_input = make_input_from_registry_value(choice);
        if (!reg_input.rtsp_url.empty() || !reg_input.usb_device.empty() || !reg_input.file_path.empty()) {
            effective_input = reg_input;
        } else {
            // Registry returned nothing useful, fallback to config
            if (!appcfg.input.rtsp_url.empty() || !appcfg.input.file_path.empty()) {
                LG_INFO("Registry empty, using argus-config.json fallback");
                if (!appcfg.input.rtsp_url.empty()) {
                    LG_INFO("  - RTSP URL: %s", appcfg.input.rtsp_url.c_str());
                } else if (!appcfg.input.file_path.empty()) {
                    LG_INFO("  - File path: %s", appcfg.input.file_path.c_str());
                }
                effective_input = appcfg.input;
            } else if (appcfg.input_source == "usb") {
                std::string detected_usb = autoDetectUsbDeviceV4L2();
                if (detected_usb.empty()) detected_usb = "/dev/video0";
                LG_INFO("Registry empty, auto-detected USB device: %s", detected_usb.c_str());
                effective_input.usb_device = detected_usb;
                effective_input.usb = appcfg.input.usb;
            } else {
                LG_INFO("No input configured, using auto-detection");
            }
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

    // Configure device ID (for MQTT messages)
    pc.device_id = appcfg.device_id;

    // Configure frame output (optional)
    pc.enable_frame_output = appcfg.enable_frame_output;
    pc.output_dir = appcfg.output_dir;
    pc.max_frames = appcfg.max_frames;
    pc.frame_quality = appcfg.frame_quality;

    // Configure face blur for privacy
    pc.blur_config.enabled = appcfg.blur_faces;
    pc.blur_config.intensity = appcfg.blur_intensity;
    if (appcfg.blur_method == "gaussian") {
        pc.blur_config.method = output::BlurMethod::GAUSSIAN;
    } else {
        pc.blur_config.method = output::BlurMethod::PIXELATE;  // Default
    }

    // Configure employee vest detection (MobileNetV3-Small classifier)
    // Prefer the new dedicated employee_detection config; fall back to legacy fields.
    const bool employee_detection_enabled = appcfg.employee_detection.enabled;
    pc.enable_employee_detection = employee_detection_enabled;
    pc.enable_uniform_model      = employee_detection_enabled;  // keep legacy flag in sync
    if (employee_detection_enabled) {
        pc.employee_model_path = appcfg.employee_detection.model_path;
        pc.employee_npu_core   = appcfg.employee_detection.npu_core;
        // Also populate legacy ModelSpec so existing orchestrator code continues to work
        pc.uniform_model.model_path    = appcfg.employee_detection.model_path;
        pc.uniform_model.npu_core      = appcfg.employee_detection.npu_core;
        pc.uniform_model.family        = ModelFamily::MobileNetV3;
        pc.uniform_model.backend       = Backend::RKNN;
        pc.uniform_model.task          = TaskType::Classifier;
        pc.uniform_model.input_size    = {224, 224};
        pc.uniform_model.input_channels = 3;
        pc.uniform_model.input_layout  = ColorLayout::RGB;
        LG_INFO("Employee vest detection enabled: model=%s npu_core=%d",
                pc.employee_model_path.c_str(), pc.employee_npu_core);
    } else {
        LG_INFO("Employee vest detection disabled");
    }

    // If frame output not configured, enable with defaults
    if (!pc.enable_frame_output) {
        pc.enable_frame_output = true;
        pc.output_dir = "/tmp";
        pc.max_frames = 1;        // Keep only latest frame (overwrite)
        pc.frame_quality = 85;
        LG_INFO("Frame output not configured, enabling defaults: dir=/tmp max_frames=1 quality=85");
    }
    
    // Copy publishers configuration
    pc.publishers = appcfg.publishers;
    LG_INFO("Configured %zu publisher(s)", pc.publishers.size());
    
    if (pc.enable_frame_output && !pc.output_dir.empty()) {
        LG_INFO("Frame output enabled: dir=%s max_frames=%d quality=%d",
                pc.output_dir.c_str(), pc.max_frames, pc.frame_quality);
    }
    LG_INFO("Selected input: usb=%s rtsp=%s file=%s",
            pc.input.usb_device.c_str(),
            pc.input.rtsp_url.c_str(),
            pc.input.file_path.c_str());
   
    // ---- MQTT broker is managed by bsext_init (standalone daemon) ----
    // No need to start embedded broker - connect to external broker instead
    LG_INFO("MQTT broker is managed externally by bsext_init on port 1883");

#ifdef DEMO_MODE_ENABLED
    // ---- Demo mode: enforce expiration date from expires.json ----
    // Search order (first found wins):
    //   1. --license-file argument (if provided)
    //   2. /storage/sd/expires.json (SD card override)
    //   3. /storage/flash/expires.json (flash override)
    //   4. /var/volatile/bsext/ext_npu_argus/expires.json (bundled default)
    std::string license_path;
    if (cli.license_file && file_exists(cli.license_file)) {
        license_path = cli.license_file;
    } else if (file_exists("/storage/sd/expires.json")) {
        license_path = "/storage/sd/expires.json";
    } else if (file_exists("/storage/flash/expires.json")) {
        license_path = "/storage/flash/expires.json";
    } else {
        license_path = "/var/volatile/bsext/ext_npu_argus/expires.json";
    }
    LG_INFO("Demo mode: using license file %s", license_path.c_str());
    DemoLicenseChecker license_checker(license_path);
    if (license_checker.check()) {
        LG_ERROR("============================================================");
        LG_ERROR("DEMO MODE EXPIRED: expiration was %s",
                 license_checker.expires_utc().c_str());
        LG_ERROR("This build is a DEMO VERSION. Obtain a new version to");
        LG_ERROR("continue operation. Contact your BrightSign representative.");
        LG_ERROR("============================================================");

        // Loop without starting the orchestrator so the service manager does
        // not immediately restart the process into a normal operation cycle.
        auto last_warning = std::chrono::steady_clock::now();
        while (!g_stop.load()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_warning).count();
            if (elapsed >= 60) {
                LG_ERROR("============================================================");
                LG_ERROR("DEMO MODE EXPIRED: video processing is disabled");
                LG_ERROR("Expiration: %s  |  Obtain a new version to continue.",
                         license_checker.expires_utc().c_str());
                LG_ERROR("============================================================");
                last_warning = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return 0;
    }

    if (!license_checker.expires_utc().empty()) {
        LG_INFO("Demo mode active — expires %s", license_checker.expires_utc().c_str());
    }

    auto last_license_check = std::chrono::steady_clock::now();
#endif  // DEMO_MODE_ENABLED

    // ---- Config monitoring for automatic restart ----
    // Monitor /storage/sd/configs/config.json for changes
    // When changed, request graceful restart to apply new config
    LG_INFO("Initializing config monitor...");
    LG_INFO("Initial flag states: g_stop=%d g_restart_requested=%d", 
            g_stop.load(), g_restart_requested.load());
    
    // ---- run ----
    LG_INFO("Starting orchestrator...");
    Orchestrator orch{pc};
    if (!orch.start()) {
        LG_ERROR("Failed to start orchestrator");
        //config_monitor.stop();
        return 1;
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    
    LG_INFO("Entering main loop...");
    LG_INFO("DEBUG: Monitoring flags - g_stop address=%p, g_restart_requested address=%p",
            (void*)&g_stop, (void*)&g_restart_requested);

    // last_license_check is declared in the DEMO_MODE_ENABLED block above.

    // Main loop: wait for stop signal or config change request
    int loop_count = 0;
    while (!g_stop.load() && !g_restart_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

#ifdef DEMO_MODE_ENABLED
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_license_check).count();
            if (elapsed >= 60) {
                last_license_check = std::chrono::steady_clock::now();
                if (license_checker.check()) {
                    LG_ERROR("============================================================");
                    LG_ERROR("DEMO MODE EXPIRED: stopping video processing");
                    LG_ERROR("Expiration: %s  |  Obtain a new version to continue.",
                             license_checker.expires_utc().c_str());
                    LG_ERROR("============================================================");
                    g_stop.store(true);
                    break;
                }
            }
        }
#endif

        // Log flag status every 30 seconds (150 iterations * 200ms = 30s)
        if (++loop_count % 150 == 0) {
            LG_INFO("DEBUG: Main loop heartbeat - g_stop=%d g_restart_requested=%d",
                    g_stop.load(), g_restart_requested.load());
        }
    }
    
    // Check why we're exiting
    LG_INFO("DEBUG: Exited main loop!");
    LG_INFO("DEBUG: Final flag states - g_stop=%d g_restart_requested=%d",
            g_stop.load(), g_restart_requested.load());
    
    if (g_restart_requested.load()) {
        LG_INFO("========================================");
        LG_INFO("RESTART REQUESTED: Shutting down to apply new config");
        LG_INFO("Service manager (systemd) will automatically restart the extension");
        LG_INFO("========================================");
    } else {
        LG_INFO("Shutdown requested via signal");
    }

    // Stop config monitor first
    LG_INFO("DEBUG: Step 1 - Stopping config monitor...");
    //config_monitor.stop();
    LG_INFO("DEBUG: Step 1 - Config monitor stopped");
    
    // Then stop orchestrator
    LG_INFO("DEBUG: Step 2 - Requesting orchestrator stop...");
    orch.request_stop();
    LG_INFO("DEBUG: Step 2 - Orchestrator stop requested");
    
    LG_INFO("DEBUG: Step 3 - Waiting for orchestrator to join (with 10 second timeout)...");
    
    // Try to join with timeout - if it takes too long, force exit
    std::atomic<bool> join_completed{false};
    std::thread join_thread([&]() {
        orch.join();
        join_completed.store(true);
    });
    
    // Wait up to 10 seconds for orchestrator to stop
    auto start = std::chrono::steady_clock::now();
    while (!join_completed.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start
        ).count();
        
        if (elapsed >= 10) {
            LG_WARN("DEBUG: Orchestrator join timeout after %ld seconds - forcing exit", elapsed);
            join_thread.detach();  // Let it run, we're exiting anyway
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (join_completed.load()) {
        join_thread.join();
        LG_INFO("DEBUG: Step 3 - Orchestrator joined successfully");
    } else {
        LG_WARN("DEBUG: Step 3 - Orchestrator did not join cleanly, forcing exit");
    }
    
    LG_INFO("Shutdown complete");

#ifdef DEMO_MODE_ENABLED
    if (license_checker.is_expired()) {
        // Reset g_stop so the warning loop runs until a real shutdown signal.
        g_stop.store(false);
        auto last_warning = std::chrono::steady_clock::now();
        while (!g_stop.load()) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - last_warning).count();
            if (elapsed >= 60) {
                LG_ERROR("============================================================");
                LG_ERROR("DEMO MODE EXPIRED: video processing is disabled");
                LG_ERROR("Expiration: %s  |  Obtain a new version to continue.",
                         license_checker.expires_utc().c_str());
                LG_ERROR("============================================================");
                last_warning = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return 0;
    }
#endif

    // Return exit code to indicate reason for exit
    // Exit code 42: Restart requested (config changed)
    // Exit code 0: Normal shutdown
    int exit_code = g_restart_requested.load() ? 42 : 0;
    LG_INFO("DEBUG: Returning exit code %d", exit_code);
    return exit_code;
}
