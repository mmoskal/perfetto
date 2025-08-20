/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law of an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/ext/base/lock_free_task_runner.h"

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/watchdog.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <poll.h>
#endif

namespace perfetto {
namespace base {

namespace {
template <typename T>
bool AllBitsSet(T v) {
  return v == static_cast<T>(-1);
}

template <typename T>
T BitwiseNot(T v) {
  return v ^ static_cast<T>(-1);
}

}  // namespace

// --- LockFreeTaskRunner::Slab ---
LockFreeTaskRunner::Slab::Slab() = default;
LockFreeTaskRunner::Slab::~Slab() = default;

// --- LockFreeTaskRunner ---
LockFreeTaskRunner::LockFreeTaskRunner() {
  static_assert((kSlabSize & (kSlabSize - 1)) == 0, "kSlabSize must be a pow2");
  head_.store(new Slab(), std::memory_order_release);
}

LockFreeTaskRunner::~LockFreeTaskRunner() {
  Slab* slab = head_.load(std::memory_order_relaxed);

  Slab* free_slab = free_slab_.load(std::memory_order_relaxed);
  if (free_slab && free_slab_.compare_exchange_strong(free_slab, nullptr)) {
    delete free_slab;
  }

  // TODO check here what the original UnixTaskRunner does.
  while (slab) {
    Slab* next = slab->next.load(std::memory_order_relaxed);
    delete slab;
    slab = next;
  }
}

void LockFreeTaskRunner::PostTask(std::function<void()> closure) {
  Slab* slab;
  for (;;) {
    slab = head_.load(std::memory_order_acquire);
    PERFETTO_DCHECK(slab);  // The head is always valid.

    // If a Slab has a non-null next pointer, it must have ran out of `tasks`
    // slots, hence move to the next one.
    while (Slab* next = slab->next.load(std::memory_order_acquire)) {
      PERFETTO_DCHECK(
          std::all_of(slab->tasks_written.begin(), slab->tasks_written.end(),
                      [](const std::atomic<Slab::BitWord>& w) {
                        return AllBitsSet(w.load(std::memory_order_relaxed));
                      }));
      slab = next;
    }

    size_t slot = slab->next_task_slot.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kSlabSize) {
      // We have two cases here:
      // 1. slot == kSlabSize (most common) : The slab was full and we tried to
      //    allocate another element. We have to allocate a new slab.
      // 2. (slot > kSlabSize) Like 1, but two (or more) threads raced on it.
      //    Only the one that reached kSlabSize should continue and allocate a
      //    new slab, the others shall repeat the search. It's okay if we leave
      //    next_task_slot over-incremented (> kSlabSize) as it's only used by
      //    the writers to find a slot.
      if (slot > kSlabSize)
        continue;  // Case 2.

      // Case 1: the slab is full and we have to allocate a new slab.

      // Allocate a new slab, trying to use the freelist first.
      Slab* new_slab = nullptr;
      Slab* free_slab = free_slab_.load(std::memory_order_relaxed);
      if (free_slab && free_slab_.compare_exchange_strong(free_slab, nullptr)) {
        new_slab = free_slab;
      } else {
        new_slab = new Slab();
      }
      PERFETTO_DCHECK(new_slab);

      slot = 0;
      new_slab->next_task_slot.store(1, std::memory_order_release);

      // Nobody else can change the next pointer. Even in case of writers racing
      // only the writer that increments to kSlabSize is the one in charge of
      // allocating the other slab. The others will spin on the for(;;) until
      // the elected one is done.
      Slab* null_slab = nullptr;
      PERFETTO_CHECK(slab->next.compare_exchange_strong(null_slab, new_slab));
      slab = new_slab;
    }

    PERFETTO_DCHECK(!slab->tasks[slot]);
    slab->tasks[slot] = std::move(closure);

    size_t s_word = slot / Slab::BitsPerWord;
    size_t s_bit = slot % Slab::BitsPerWord;
    size_t s_mask = size_t(1) << s_bit;
    PERFETTO_DCHECK((slab->tasks_written[s_word] & s_mask) == 0);
    slab->tasks_written[s_word].fetch_or(s_mask, std::memory_order_release);
    return;
  }  // for(;;)
}

void LockFreeTaskRunner::Run() {
  while (!quit_.load(std::memory_order_relaxed)) {
    EnqueueExpiredDelayedTasks();
    std::function<void()> imm_task = PopNextImmediateTask();
    if (imm_task) {
      errno = 0;
      RunTaskWithWatchdogGuard(imm_task);
      continue;
    }
  }
}

std::function<void()> LockFreeTaskRunner::PopNextImmediateTask() {
  using BitWord = Slab::BitWord;
  Slab* slab = head_.load(std::memory_order_acquire);
  for (size_t slab_idx = 0;; ++slab_idx) {
    size_t words_fully_consumed = 0;
    constexpr size_t kNumWordsPerSlab = kSlabSize / Slab::BitsPerWord;
    for (size_t w = 0; w < kNumWordsPerSlab; ++w) {
      BitWord wr_word = slab->tasks_written[w].load(std::memory_order_acquire);
      BitWord rd_word = slab->tasks_read[w];
      words_fully_consumed += AllBitsSet(rd_word) ? 1 : 0;
      BitWord unread_word = wr_word & BitwiseNot(rd_word);

      if (unread_word == 0)
        continue;  // Not load bearing, an optimization to avoid bitscanning.

      // TODO use __builtin_clzll (or maybe count tail).
      for (size_t bit = 0; bit < Slab::BitsPerWord; ++bit) {
        BitWord bit_mask = BitWord(1) << bit;
        if (unread_word & bit_mask) {
          size_t slot = w * Slab::BitsPerWord + bit;
          std::function<void()> task = std::move(slab->tasks[slot]);
          slab->tasks[slot] = nullptr;
          slab->tasks_read[w] |= bit_mask;
          return task;
        }
      }  // for(bit)
    }  // for(word in tasks_written)

    // There are no more unread tasks in the Slab. The slab might or might not
    // be full. If full we should delete it now.

    // TODO think more here about all cases, especially the race when there are
    // no slots to read, but the slab is not full.

    Slab* next_slab = slab->next.load(std::memory_order_acquire);
    if (words_fully_consumed == kNumWordsPerSlab && slab_idx == 0) {
      // If we have read all the slots, it means that they are also all written,
      // which in turn means there can't possibly be any writer still using the
      // slab, as writing the tasks_written is the very last operation that
      // PostTask() does before returning. It's now safe to delete the slab.
      slab->Reset();
      if (next_slab) {
        PERFETTO_DCHECK(head_.load(std::memory_order_relaxed) == slab);
        head_.store(next_slab, std::memory_order_release);
        DeleteSlab(slab);
      } else {
        // We consumed the current slab, and there is no next slab. We can't
        // leave the head_ null, so we just recycle the loundered slab (there is
        // no point putting into the freelist and then immediately popping it
        // back).
        PERFETTO_DCHECK(head_.load(std::memory_order_relaxed) == slab);
        return nullptr;
      }
    }

    slab = next_slab;
  }  // for(slab)
}

void LockFreeTaskRunner::DeleteSlab(Slab* slab) {
  // DCHECK main thread
  Slab* null_slab = nullptr;
  if (free_slab_.compare_exchange_strong(null_slab, slab))
    return;
  delete slab;
}

void LockFreeTaskRunner::Quit() {
  quit_.store(true, std::memory_order_relaxed);
  WakeUp();
}

void LockFreeTaskRunner::WakeUp() {
  event_.Notify();
}

void LockFreeTaskRunner::PostDelayedTask(std::function<void()>, uint32_t) {
  PERFETTO_FATAL("Not implemented");
}

void LockFreeTaskRunner::AddFileDescriptorWatch(PlatformHandle,
                                                std::function<void()>) {
  PERFETTO_FATAL("Not implemented");
}

void LockFreeTaskRunner::RemoveFileDescriptorWatch(PlatformHandle) {
  PERFETTO_FATAL("Not implemented");
}

bool LockFreeTaskRunner::RunsTasksOnCurrentThread() const {
  return false;
}

}  // namespace base
}  // namespace perfetto