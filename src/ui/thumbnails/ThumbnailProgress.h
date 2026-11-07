#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace SkyBuild::UI::ThumbnailGenerator {
inline constexpr std::string_view PICKER_PREVIEW_PATH =
    "thumbnails/.picker-preview.png";
struct Progress {
  std::uint64_t generation{0};
  std::size_t total{0}, completed{0}, failed{0};
  bool running{false}, cancelling{false}, cancelled{false};
  std::string status;
};
class ProgressTracker {
public:
  void Add(std::size_t count) {
    if (!count)
      return;
    std::scoped_lock lock(mutex);
    if (!progress.running) {
      const auto generation = progress.generation + 1;
      progress = {};
      progress.generation = generation;
    }
    progress.total += count;
    progress.running = true;
    progress.status = "Preparing icons...";
  }
  void Finish(bool success) {
    std::scoped_lock lock(mutex);
    if (!progress.running)
      return;
    if (progress.cancelling) {
      if (success)
        ++progress.completed;
      return;
    }
    ++progress.completed;
    if (!success)
      ++progress.failed;
    if (progress.completed == progress.total) {
      progress.running = false;
      progress.status = progress.failed ? "Generation finished with errors."
                                        : "Icon generation complete.";
    }
  }
  void RequestCancel() {
    std::scoped_lock lock(mutex);
    if (!progress.running)
      return;
    progress.cancelling = true;
    progress.status = "Cancelling icon generation...";
  }
  void FinishCancel() {
    std::scoped_lock lock(mutex);
    if (!progress.cancelling)
      return;
    progress.running = false;
    progress.cancelling = false;
    progress.cancelled = true;
    progress.status = "Icon generation cancelled.";
  }
  void SetStatus(std::string_view status) {
    std::scoped_lock lock(mutex);
    if (progress.running && !progress.cancelling)
      progress.status = status;
  }
  Progress Get() const {
    std::scoped_lock lock(mutex);
    return progress;
  }

private:
  mutable std::mutex mutex;
  Progress progress;
};
} // namespace SkyBuild::UI::ThumbnailGenerator
