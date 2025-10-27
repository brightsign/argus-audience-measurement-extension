#include "output/file_sinks.h"

struct FileDebugSink::Impl { FileSinkConfig cfg; bool started{false}; Impl(FileSinkConfig c):cfg(std::move(c)){} };

FileDebugSink::FileDebugSink(const FileSinkConfig& cfg) noexcept : p_(new Impl(cfg)) {}
FileDebugSink::~FileDebugSink() = default;

bool FileDebugSink::start() noexcept { p_->started = true; return true; }
void FileDebugSink::stop() noexcept { p_->started = false; }
bool FileDebugSink::write_annotated(const ImageBuffer&, const PipelineResult&, const AnnotationSpec&) noexcept { return p_->started; }
bool FileDebugSink::write_perf(const LogRecord&) noexcept { return p_->started; }
bool FileDebugSink::dump_frame_raw(const void*, size_t, const char*) noexcept { return p_->started; }

