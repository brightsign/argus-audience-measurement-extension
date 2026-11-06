// SPDX-License-Identifier: MIT
// Keeps argus_display_bridge running next to vm_api_server (only when visual_memory.display.enabled). The bridge is the single component
// that links gRPC; running it as a child means no change to the extension's start scripts and a clean stop with the API process.
#pragma once

#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace argus_display {

class BridgeSupervisor {
public:
    BridgeSupervisor(std::string binary, std::vector<std::string> args) : bin_(std::move(binary)), args_(std::move(args)) {}
    ~BridgeSupervisor() { stop(); }

    void start() {
        if (th_.joinable()) return;
        stop_ = false;
        th_ = std::thread([this] { loop(); });
    }
    void stop() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        terminate_child();
    }
    nlohmann::json status() {
        std::lock_guard<std::mutex> lk(mu_);
        return {{"managed", true}, {"binary", bin_}, {"binary_present", std::filesystem::exists(bin_)}, {"running", pid_ > 0}, {"pid", pid_ > 0 ? nlohmann::json(pid_) : nlohmann::json(nullptr)},
                {"restarts", restarts_}, {"last_exit", last_exit_}};
    }

private:
    void spawn() {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin_.c_str()));
        for (auto& a : args_) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const pid_t p = ::fork();
        if (p == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGTERM);                   // never outlive vm_api_server
            ::execv(bin_.c_str(), argv.data());
            ::_exit(127);
        }
        std::lock_guard<std::mutex> lk(mu_);
        pid_ = p > 0 ? p : -1;
        started_ = std::chrono::steady_clock::now();
    }
    void terminate_child() {
        pid_t p;
        { std::lock_guard<std::mutex> lk(mu_); p = pid_; pid_ = -1; }
        if (p <= 0) return;
        ::kill(p, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int st;
            if (::waitpid(p, &st, WNOHANG) == p) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ::kill(p, SIGKILL);
        int st; ::waitpid(p, &st, 0);
    }
    void loop() {
        int backoff = 1000;
        while (!stop_) {
            pid_t p;
            { std::lock_guard<std::mutex> lk(mu_); p = pid_; }
            if (p <= 0) {
                if (!std::filesystem::exists(bin_)) { sleep_ms(2000); continue; }       // not packaged on this image: stay quiet, report it in the status
                spawn();
                sleep_ms(200);
                continue;
            }
            int st = 0;
            if (::waitpid(p, &st, WNOHANG) == p) {
                const auto ran = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
                { std::lock_guard<std::mutex> lk(mu_); pid_ = -1; ++restarts_; last_exit_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st); }
                backoff = ran > 5000 ? 1000 : std::min(30000, backoff * 2);
                sleep_ms(backoff);
                continue;
            }
            sleep_ms(250);
        }
    }
    void sleep_ms(int ms) { for (int i = 0; i < ms / 50 && !stop_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

    std::string bin_;
    std::vector<std::string> args_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
    pid_t pid_ = -1;
    int restarts_ = 0, last_exit_ = 0;
    std::chrono::steady_clock::time_point started_;
};

}  // namespace argus_display
