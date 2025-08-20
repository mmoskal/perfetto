/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_EXT_BASE_LOCK_FREE_TASK_RUNNER_H_
#define INCLUDE_PERFETTO_EXT_BASE_LOCK_FREE_TASK_RUNNER_H_

#include "perfetto/base/task_runner.h"
#include "perfetto/base/thread_annotations.h"
#include "perfetto/ext/base/event_fd.h"

#include <array>
#include <atomic>
#include <functional>
#include <vector>

namespace perfetto {
namespace base {

class PERFETTO_EXPORT_COMPONENT LockFreeTaskRunner : public TaskRunner {
 public:
  LockFreeTaskRunner();
  ~LockFreeTaskRunner() override;

  void Run();
  void Quit();

  // TaskRunner implementation:
  void PostTask(std::function<void()>) override;
  void PostDelayedTask(std::function<void()>, uint32_t delay_ms) override;
  void AddFileDescriptorWatch(PlatformHandle, std::function<void()>) override;
  void RemoveFileDescriptorWatch(PlatformHandle) override;
  bool RunsTasksOnCurrentThread() const override;

 private:
  static constexpr size_t kSlabSize = 1024;

  struct Slab {
    Slab();
    ~Slab();

    void Reset() {
      this->~Slab();
      new (this) Slab();
    }

    // This is manipulated only by the writer threads. The main thread only
    // accesses tasks[task_idx[N]].
    std::array<std::function<void()>, kSlabSize> tasks;
    std::atomic<size_t> next_task_slot{0};

    // TODO comment.
    using BitWord = size_t;
    static constexpr size_t BitsPerWord = sizeof(BitWord) * 8;

    std::array<std::atomic<BitWord>, kSlabSize / BitsPerWord> tasks_written;
    std::array<BitWord, kSlabSize / BitsPerWord> tasks_read;

    std::atomic<Slab*> next{nullptr};
  };

  std::function<void()> PopNextImmediateTask();
  void WakeUp();

  void DeleteSlab(Slab* slab);

  alignas(64) std::atomic<Slab*> head_;

  // This is semantically a unique_ptr, but is accessed from different threads.
  std::atomic<Slab*> free_slab_;

  EventFd event_;
  std::atomic<bool> quit_{false};
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_EXT_BASE_LOCK_FREE_TASK_RUNNER_H_
