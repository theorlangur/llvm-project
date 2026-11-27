//===--- PCHManagerQueue.cpp - Priority queue for PCHManager ----*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PCHManager.h"
#include "support/Logger.h"
#include <algorithm>
#include <cassert>

namespace clang {
namespace clangd {

void PCHQueue::notifyProgress() const {
  dlog("PCHQueue: {0}/{1} ({2} active). Last idle at {3}", Stat.Completed,
       Stat.Enqueued, Stat.Active, Stat.LastIdle);
  if (OnProgress)
    OnProgress(Stat);
}

void PCHQueue::work(std::function<void()> OnIdle) {
  while (true) {
    std::optional<Task> NextTask;
    {
      std::unique_lock<std::mutex> Lock(Mu);
      CV.wait(Lock, [&] { return ShouldStop || !Queue.empty(); });
      if (ShouldStop) {
        Queue.clear();
        CV.notify_all();
        return;
      }
      ++Stat.Active;
      std::pop_heap(Queue.begin(), Queue.end());
      NextTask = std::move(Queue.back());
      Queue.pop_back();
      notifyProgress();
    }

    if (NextTask->ThreadPri != llvm::ThreadPriority::Default)
      llvm::set_thread_priority(NextTask->ThreadPri);
    NextTask->Run();
    if (NextTask->ThreadPri != llvm::ThreadPriority::Default)
      llvm::set_thread_priority(llvm::ThreadPriority::Default);

    {
      std::unique_lock<std::mutex> Lock(Mu);
      ++Stat.Completed;
      if (Stat.Active == 1 && Queue.empty()) {
        assert(ShouldStop || Stat.Completed == Stat.Enqueued);
        Stat.LastIdle = Stat.Completed;
        if (OnIdle) {
          Lock.unlock();
          OnIdle();
          Lock.lock();
        }
      }
      assert(Stat.Active > 0 && "before decrementing");
      --Stat.Active;
      notifyProgress();
    }
    CV.notify_all();
  }
}

void PCHQueue::stop() {
  {
    std::lock_guard<std::mutex> QueueLock(Mu);
    ShouldStop = true;
  }
  CV.notify_all();
}

void PCHQueue::push(Task T) {
  {
    std::lock_guard<std::mutex> Lock(Mu);
    Queue.push_back(std::move(T));
    std::push_heap(Queue.begin(), Queue.end());
    ++Stat.Enqueued;
    notifyProgress();
  }
  CV.notify_all();
}

void PCHQueue::append(std::vector<Task> Tasks) {
  {
    std::lock_guard<std::mutex> Lock(Mu);
    for (Task &T : Tasks) {
      Queue.push_back(std::move(T));
      ++Stat.Enqueued;
    }
    std::make_heap(Queue.begin(), Queue.end());
    notifyProgress();
  }
  CV.notify_all();
}

bool PCHQueue::blockUntilIdle(Deadline D) const {
  std::unique_lock<std::mutex> Lock(Mu);
  return clangd::wait(Lock, CV, D, [&] {
    return ShouldStop || (Stat.Active == 0 && Queue.empty());
  });
}

} // namespace clangd
} // namespace clang
