#include <atomic>
#include <csignal>
#include <chrono>
#include <thread>
#include <iostream>

#include "metrics/file_logger.h"
#include "metrics/log_global.h"
#include "config/configuration.h"
#include "orchestration/orchestrator.h"
#include "input/input_from_registry.h"
#include "input/registry_helper.h"
#include "util/util.h"

static std::atomic<bool> g_stop{false};
static void on_sig(int){ g_stop.store(true); }

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
    pc.model = appcfg.primary_model;

    LG_INFO("Selected input: usb=%s rtsp=%s file=%s",
            pc.input.usb_device.c_str(),
            pc.input.rtsp_url.c_str(),
            pc.input.file_path.c_str());
   
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
