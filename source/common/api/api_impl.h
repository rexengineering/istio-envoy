#pragma once

#include <chrono>
#include <string>

#include "envoy/api/api.h"
#include "envoy/event/timer.h"
#include "envoy/filesystem/filesystem.h"
#include "envoy/thread/thread.h"

namespace Envoy {
namespace Api {

/**
 * Implementation of Api::Api
 */
class Impl : public Api::Api {
public:
  Impl(std::chrono::milliseconds file_flush_interval_msec, Thread::ThreadFactory& thread_factory);

  // Api::Api
  Event::DispatcherPtr allocateDispatcher(Event::TimeSystem& time_system) override;
  Filesystem::FileSharedPtr createFile(const std::string& path, Event::Dispatcher& dispatcher,
                                       Thread::BasicLockable& lock,
                                       Stats::Store& stats_store) override;
  bool fileExists(const std::string& path) override;
  std::string fileReadToEnd(const std::string& path) override;
  Thread::ThreadPtr createThread(std::function<void()> thread_routine) override;

private:
  std::chrono::milliseconds file_flush_interval_msec_;
  Thread::ThreadFactory& thread_factory_;
};

} // namespace Api
} // namespace Envoy
