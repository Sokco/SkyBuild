#include "SkyBuildPaths.h"
#include "log.h"

#include "Plugin.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace Logging {
void Setup() {
  auto logDirectory = SkyBuild::Paths::DiagnosticsDirectory();

  if (!logDirectory) {
    SKSE::stl::report_and_fail("Unable to create Data/SkyBuild/logs.");
  }

  *logDirectory /= std::string(Plugin::Name) + ".log";

  auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      logDirectory->string(), true);

  auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));

#ifdef NDEBUG
  constexpr auto logLevel = spdlog::level::info;
#else
  constexpr auto logLevel = spdlog::level::trace;
#endif

  logger->set_level(logLevel);
  // Routine messages are buffered; failures flush immediately.
  logger->flush_on(spdlog::level::warn);

  spdlog::set_default_logger(std::move(logger));

  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
}
} // namespace Logging
