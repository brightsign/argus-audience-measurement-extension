#include "config/configuration.h"
#include <cstdio>
#include <fstream>
#include <3rdparty/nlohmann/json.hpp>

using json = nlohmann::json;

bool AppConfig::validate(char* err, size_t err_sz) const noexcept {
  char tmp[128]{};
  if (!primary_model.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "model invalid: %s", tmp);
    return false;
  }
  if (!processing.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "processing invalid: %s", tmp);
    return false;
  }
  if (!runtime.validate(tmp, sizeof(tmp))) {
    if (err&&err_sz) std::snprintf(err, err_sz, "runtime invalid: %s", tmp);
    return false;
  }
  for (const auto& p : publishers) {
    if (!p.validate(tmp, sizeof(tmp))) {
      if (err&&err_sz) std::snprintf(err, err_sz, "publisher invalid: %s", tmp);
      return false;
    }
  }
  return true;
}

namespace config {
bool load_from_file(const std::string& path, AppConfig& out, bool /*strict*/, char* err, size_t err_sz) noexcept {
  // Validate path
  if (path.empty()) { 
    if (err && err_sz) std::snprintf(err, err_sz, "empty path"); 
    return false; 
  }
  
  try {
    // Read JSON file
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
      if (err && err_sz) std::snprintf(err, err_sz, "cannot open file: %s", path.c_str());
      return false;
    }
    
    json j;
    ifs >> j;
    ifs.close();
    
    // Parse test mode flags (top-level)
    if (j.contains("test_face_only") && j["test_face_only"].is_boolean()) {
      out.test_face_only = j["test_face_only"].get<bool>();
    }
    if (j.contains("test_yolo_only") && j["test_yolo_only"].is_boolean()) {
      out.test_yolo_only = j["test_yolo_only"].get<bool>();
    }
    
    // Parse other config sections as before (minimal for now)
    if (j.contains("primary_model")) {
      const auto& pm = j["primary_model"];
      if (pm.contains("model_path")) out.primary_model.model_path = pm["model_path"];
      if (pm.contains("name")) out.primary_model.name = pm["name"];
      if (pm.contains("input_size")) {
        if (pm["input_size"].contains("w")) out.primary_model.input_size.w = pm["input_size"]["w"];
        if (pm["input_size"].contains("h")) out.primary_model.input_size.h = pm["input_size"]["h"];
      }
      if (pm.contains("norm")) {
        if (pm["norm"].contains("channels")) out.primary_model.norm.channels = pm["norm"]["channels"];
      }
      if (pm.contains("conf_threshold")) out.primary_model.conf_threshold = pm["conf_threshold"];
      if (pm.contains("nms_threshold")) out.primary_model.nms_threshold = pm["nms_threshold"];
      if (pm.contains("npu_core")) out.primary_model.npu_core = pm["npu_core"].get<int>();
    }
    
    // Parse secondary models
    if (j.contains("secondary_models") && j["secondary_models"].is_array()) {
      for (const auto& sm : j["secondary_models"]) {
        ModelSpec secondary_spec;
        if (sm.contains("model_path")) secondary_spec.model_path = sm["model_path"];
        if (sm.contains("name")) secondary_spec.name = sm["name"];
        if (sm.contains("input_size")) {
          if (sm["input_size"].contains("w")) secondary_spec.input_size.w = sm["input_size"]["w"];
          if (sm["input_size"].contains("h")) secondary_spec.input_size.h = sm["input_size"]["h"];
        }
        if (sm.contains("norm")) {
          if (sm["norm"].contains("channels")) secondary_spec.norm.channels = sm["norm"]["channels"];
        }
        if (sm.contains("conf_threshold")) secondary_spec.conf_threshold = sm["conf_threshold"];
        if (sm.contains("nms_threshold")) secondary_spec.nms_threshold = sm["nms_threshold"];
        if (sm.contains("npu_core")) secondary_spec.npu_core = sm["npu_core"].get<int>();
        out.secondary_models.push_back(secondary_spec);
      }
    }
    
    if (j.contains("processing")) {
      const auto& proc = j["processing"];
      if (proc.contains("th")) {
        if (proc["th"].contains("score")) out.processing.th.score = proc["th"]["score"];
        if (proc["th"].contains("iou")) out.processing.th.iou = proc["th"]["iou"];
      }
    }
    
    if (j.contains("runtime")) {
      const auto& rt = j["runtime"];
      if (rt.contains("heartbeat_ms")) out.runtime.heartbeat_ms = rt["heartbeat_ms"];
    }
    
    if (j.contains("input")) {
      const auto& in = j["input"];
      if (in.contains("rtsp_url")) out.input.rtsp_url = in["rtsp_url"];
      if (in.contains("usb_device")) out.input.usb_device = in["usb_device"];
      if (in.contains("file_path")) out.input.file_path = in["file_path"];
    }
    
    return true;
  } catch (const std::exception& ex) {
    if (err && err_sz) std::snprintf(err, err_sz, "JSON parse error: %s", ex.what());
    return false;
  }
}
bool save_to_file(const std::string& /*path*/, const AppConfig& /*in*/, char* /*err*/, size_t /*err_sz*/) noexcept {
  return false;
}
}

