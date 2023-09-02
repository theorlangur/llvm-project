#include "PCHManager.h"
#include "support/Logger.h"
#include <memory>

namespace clang {
namespace clangd {

void PCHQueue::notifyProgress() const {
  dlog("Queue: {0}/{1} ({2} active). Last idle at {3}", Stat.Completed,
       Stat.Enqueued, Stat.Active, Stat.LastIdle);
  if (OnProgress)
    OnProgress(Stat);
}

void PCHQueue::work(std::function<void()> OnIdle) {
  while (true) {
    std::optional<Task> Task;
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
      Task = std::move(Queue.back());
      Queue.pop_back();
      notifyProgress();
    }

    if (Task->ThreadPri != llvm::ThreadPriority::Default)
      llvm::set_thread_priority(Task->ThreadPri);
    Task->Run();
    if (Task->ThreadPri != llvm::ThreadPriority::Default)
      llvm::set_thread_priority(llvm::ThreadPriority::Default);

    {
      std::unique_lock<std::mutex> Lock(Mu);
      ++Stat.Completed;
      if (Stat.Active == 1 && Queue.empty()) {
        // We just finished the last item, the queue is going idle.
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

} // namespace clangd
} // namespace clang
