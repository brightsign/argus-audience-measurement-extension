#include "output/mqtt_broker.h"
#include "metrics/log_global.h"
#include <chrono>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>

MqttBroker::MqttBroker(const Cfg& cfg) noexcept : cfg_(cfg) {
}

MqttBroker::~MqttBroker() {
  stop();
}

bool MqttBroker::start() noexcept {
  if (broker_pid_ > 0) return true;  // Already running

  // Build list of paths to search for mosquitto binary
  std::vector<std::string> search_paths;
  
  // Add system standard paths
  search_paths.push_back("/usr/bin/mosquitto");
  search_paths.push_back("/usr/local/bin/mosquitto");
  
  // Add paths relative to current working directory
  search_paths.push_back("./bin/mosquitto");
  search_paths.push_back("../bin/mosquitto");
  search_paths.push_back("bin/mosquitto");
  
  // Try to find mosquitto binary
  const char* mosquitto_path = nullptr;
  for (const auto& path : search_paths) {
    if (access(path.c_str(), X_OK) == 0) {
      mosquitto_path = path.c_str();
      LG_INFO("mqtt_broker: found mosquitto at %s", mosquitto_path);
      break;
    }
  }
  
  // If not found in standard paths, try PATH
  if (!mosquitto_path) {
    LG_WARN("mqtt_broker: mosquitto binary not found in standard paths, trying PATH");
    mosquitto_path = "mosquitto";  // Will search PATH via execlp
  }

  // Launch the mosquitto daemon binary
  pid_t pid = fork();
  if (pid < 0) {
    LG_ERROR("mqtt_broker: fork failed");
    return false;
  }

  if (pid == 0) {
    // Child process: exec mosquitto
    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", cfg_.port);
    
    // Redirect stdout/stderr to /dev/null to keep them quiet
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    
    // Execute mosquitto with port and bind address
    // Use execl for absolute/relative paths, execlp for PATH search
    if (mosquitto_path[0] == '.' || mosquitto_path[0] == '/') {
      // Relative or absolute path - use execl
      execl(mosquitto_path, "mosquitto",
            "-p", port_str,
            "-l", cfg_.bind_address.c_str(),
            nullptr);
    } else {
      // Search in PATH - use execlp
      execlp(mosquitto_path, "mosquitto", 
             "-p", port_str,
             "-l", cfg_.bind_address.c_str(),
             nullptr);
    }
    
    // If exec fails, exit child with error
    LG_ERROR("mqtt_broker: exec mosquitto failed (path=%s, errno=%d)", mosquitto_path, errno);
    exit(1);
  }

  // Parent process: store child PID
  broker_pid_ = pid;
  running_.store(true, std::memory_order_release);
  
  // Give broker a moment to start
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  LG_INFO("mqtt_broker: started mosquitto (pid=%d) on %s:%d", 
          broker_pid_, cfg_.bind_address.c_str(), cfg_.port);
  return true;
}

void MqttBroker::stop() noexcept {
  if (broker_pid_ <= 0) return;

  running_.store(false, std::memory_order_release);

  // Send SIGTERM to broker process
  if (kill(broker_pid_, SIGTERM) == 0) {
    // Wait for graceful shutdown
    int status;
    for (int i = 0; i < 10; i++) {
      pid_t result = waitpid(broker_pid_, &status, WNOHANG);
      if (result == broker_pid_) {
        break;  // Process exited
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // If still running, force kill
    if (kill(broker_pid_, 0) == 0) {
      kill(broker_pid_, SIGKILL);
      waitpid(broker_pid_, &status, 0);
    }
  }

  broker_pid_ = -1;
  LG_INFO("mqtt_broker: stopped");
}

void MqttBroker::broker_loop() noexcept {
  // Monitor the broker process
  while (running_.load(std::memory_order_acquire) && broker_pid_ > 0) {
    int status;
    pid_t result = waitpid(broker_pid_, &status, WNOHANG);
    if (result == broker_pid_) {
      LG_WARN("mqtt_broker: broker process exited unexpectedly");
      broker_pid_ = -1;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}
