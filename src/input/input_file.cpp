#include "input/input_file.h"
#include <atomic>

struct FileInputSource::Impl {
  std::string path;
  FileOptions opts;
  std::atomic<bool> opened{false};
  std::atomic<bool> running{false};
  HealthInfo health{};
};

FileInputSource::FileInputSource(std::string path, FileOptions opts)
: p_(new Impl{std::move(path), opts}) {}
FileInputSource::~FileInputSource() = default;

bool FileInputSource::open() noexcept { p_->opened.store(true); return true; }
bool FileInputSource::start() noexcept { if (!p_->opened) return false; p_->running.store(true); return true; }
void FileInputSource::stop() noexcept { p_->running.store(false); }
void FileInputSource::close() noexcept { p_->opened.store(false); }

FetchStatus FileInputSource::tryFetch(FrameView& out) noexcept {
  (void)out;
  if (!p_->running.load()) return FetchStatus::Timeout;
  return FetchStatus::Timeout;
}

FetchStatus FileInputSource::fetch(FrameView& out, int timeout_ms) noexcept {
  // No real frames yet; simulate waiting until timeout then return Timeout
  (void)out;
  (void)timeout_ms;
  return FetchStatus::Timeout;
}

HealthInfo FileInputSource::getHealth() const noexcept { return p_->health; }

