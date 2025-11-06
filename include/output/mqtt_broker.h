#pragma once
#include <thread>
#include <atomic>
#include <string>
#include <cstdio>
#include <fcntl.h>

struct MqttBrokerCfg {
  int port{1883};
  std::string bind_address{"0.0.0.0"};
  bool allow_anonymous{true};
};

/**
 * MQTT Broker wrapper that spawns and manages the mosquitto daemon process.
 * 
 * This class:
 * - Forks a child process to run the mosquitto broker binary
 * - Provides graceful startup/shutdown
 * - Monitors the broker process
 * 
 * The mosquitto binary is expected to be in PATH or in the same directory as the app.
 */
class MqttBroker {
public:
  using Cfg = MqttBrokerCfg;

  explicit MqttBroker(const Cfg& cfg) noexcept;
  ~MqttBroker();

  bool start() noexcept;
  void stop() noexcept;
  bool is_running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
  void broker_loop() noexcept;

  Cfg cfg_;
  pid_t broker_pid_{-1};
  std::thread broker_thread_;
  std::atomic<bool> running_{false};
};
