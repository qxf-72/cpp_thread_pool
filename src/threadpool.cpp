#include "threadpool.h"

#include <algorithm>

namespace {
// 默认任务队列容量。
constexpr std::size_t TASK_MAX_THRESHOLD = 1024;

// cached 模式默认最大线程数。
constexpr std::size_t THREAD_SIZE_THRESHOLD = 16;

// cached 模式下，多出来的线程默认空闲 60 秒后退出。
constexpr auto THREAD_MAX_IDLE_TIME = std::chrono::seconds(60);
} // namespace

ThreadPool::ThreadPool()
    : initThreadSize_(0), taskQueMaxThreshold_(TASK_MAX_THRESHOLD),
      threadSizeThreshold_(THREAD_SIZE_THRESHOLD), currentThreadSize_(0),
      idleThreadSize_(0), threadMaxIdleTime_(THREAD_MAX_IDLE_TIME),
      poolMode_(ThreadPoolMode::MODE_FIXED), isPoolRunning_(false),
      isShuttingDown_(false) {}

ThreadPool::~ThreadPool() {
  // 析构时采用和用户显式调用相同的关闭流程：
  // 停止接收新任务，等待已入队任务完成，然后回收工作线程。
  shutdown();
}

void ThreadPool::shutdown() {
  std::vector<Worker> workers;

  {
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // shutdown() 允许被重复调用。若另一个线程正在关闭，这里等待它完成即可。
    if (isShuttingDown_) {
      notEmpty_.wait(lock, [this] { return !isShuttingDown_; });
      return;
    }

    // 已经关闭且没有线程需要回收，直接返回。
    if (!isPoolRunning_ && threads_.empty()) {
      return;
    }

    // 关闭后不再接收新任务；队列中已有任务仍会被工作线程处理完。
    isShuttingDown_ = true;
    isPoolRunning_ = false;
  }

  // 唤醒工作线程和可能阻塞在 submit() 中的提交线程，让它们重新检查状态。
  notEmpty_.notify_all();
  notFull_.notify_all();

  {
    std::lock_guard<std::mutex> lock(taskQueMtx_);
    // 把线程对象移出成员容器，在锁外 join，避免长时间持锁等待。
    workers.swap(threads_);
  }

  const auto currentThreadId = std::this_thread::get_id();
  for (auto &worker : workers) {
    if (!worker.thread.joinable()) {
      continue;
    }
    if (worker.thread.get_id() == currentThreadId) {
      // 防御性处理：如果用户任务内部调用 shutdown()，不能 join 当前线程。
      worker.thread.detach();
    } else {
      worker.thread.join();
    }
  }

  {
    std::lock_guard<std::mutex> lock(taskQueMtx_);
    // 所有线程已经结束，统计值归零；后续 submit() 会因为未运行而失败。
    currentThreadSize_ = 0;
    idleThreadSize_ = 0;
    isShuttingDown_ = false;
  }

  // 唤醒等待 shutdown() 完成的线程。
  notEmpty_.notify_all();
}

void ThreadPool::setMode(ThreadPoolMode poolMode) {
  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_ || isShuttingDown_) {
    throw std::runtime_error("Cannot change thread pool mode after start");
  }
  poolMode_ = poolMode;
}

void ThreadPool::setTaskQueMaxThreshold(std::size_t threshold) {
  if (threshold == 0) {
    throw std::invalid_argument("Task queue threshold must be greater than 0");
  }

  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_ || isShuttingDown_) {
    throw std::runtime_error("Cannot change task queue threshold after start");
  }
  taskQueMaxThreshold_ = threshold;
}

void ThreadPool::setThreadSizeThreshold(std::size_t threshold) {
  if (threshold == 0) {
    throw std::invalid_argument("Thread size threshold must be greater than 0");
  }

  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_ || isShuttingDown_) {
    throw std::runtime_error("Cannot change thread size threshold after start");
  }
  threadSizeThreshold_ = threshold;
}

void ThreadPool::setThreadMaxIdleTime(std::chrono::seconds idleTime) {
  if (idleTime.count() <= 0) {
    throw std::invalid_argument("Thread max idle time must be greater than 0");
  }

  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_ || isShuttingDown_) {
    throw std::runtime_error("Cannot change thread idle time after start");
  }
  threadMaxIdleTime_ = idleTime;
}

std::size_t ThreadPool::currentThreadSize() const {
  std::lock_guard<std::mutex> lock(taskQueMtx_);
  return currentThreadSize_;
}

std::size_t ThreadPool::idleThreadSize() const {
  std::lock_guard<std::mutex> lock(taskQueMtx_);
  return idleThreadSize_;
}

void ThreadPool::start(std::size_t initThreadSize) {
  if (initThreadSize == 0) {
    throw std::invalid_argument("Initial thread size must be greater than 0");
  }

  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_ || isShuttingDown_) {
    throw std::runtime_error("Thread pool has already been started");
  }

  initThreadSize_ = initThreadSize;

  // 如果用户设置的最大线程数比初始线程数还小，这里自动抬高到初始线程数。
  // 否则 cached 模式会出现“刚启动就超过最大线程数”的矛盾状态。
  threadSizeThreshold_ = std::max(threadSizeThreshold_, initThreadSize_);
  isPoolRunning_ = true;

  // 提前 reserve 可以减少 cached 扩容时 vector 扩容带来的移动成本。
  threads_.reserve(threadSizeThreshold_);
  for (std::size_t i = 0; i < initThreadSize_; ++i) {
    addThreadLocked();
  }
}

void ThreadPool::addThreadLocked() {
  // 先增加计数，再启动线程；如果创建线程失败，在 catch 中把计数回滚。
  threads_.emplace_back();
  auto &worker = threads_.back();
  worker.state = std::make_shared<WorkerState>();
  ++currentThreadSize_;
  try {
    auto state = worker.state;
    worker.thread = std::thread([this, state] { threadFunc(state); });
  } catch (...) {
    --currentThreadSize_;
    threads_.pop_back();
    throw;
  }
}

void ThreadPool::reapFinishedThreadsLocked(std::unique_lock<std::mutex> &lock) {
  for (;;) {
    // 每次从头查找一个已退出线程。join 期间会临时释放锁，
    // 因此不能依赖释放锁前保存的迭代器继续遍历。
    auto it = std::find_if(threads_.begin(), threads_.end(),
                           [](const Worker &worker) {
                             return worker.state && worker.state->finished;
                           });
    if (it == threads_.end()) {
      return;
    }

    std::thread thread = std::move(it->thread);
    it = threads_.erase(it);

    // join 可能阻塞，必须放到锁外执行，避免影响 submit()/shutdown()。
    lock.unlock();
    if (thread.joinable()) {
      if (thread.get_id() == std::this_thread::get_id()) {
        thread.detach();
      } else {
        thread.join();
      }
    }
    lock.lock();
  }
}

void ThreadPool::threadFunc(std::shared_ptr<WorkerState> state) {
  // 只能在持有 taskQueMtx_ 时调用：它同时修改线程统计值和 finished 标记。
  auto finishThread = [this, state] {
    if (currentThreadSize_ > 0) {
      --currentThreadSize_;
    }
    state->finished = true;
  };

  for (;;) {
    std::function<void()> task;

    {
      std::unique_lock<std::mutex> lock(taskQueMtx_);

      // 线程进入等待区，说明当前没有立即执行用户任务，先记为空闲。
      ++idleThreadSize_;

      while (taskQue_.empty() && isPoolRunning_) {
        if (poolMode_ == ThreadPoolMode::MODE_CACHED &&
            currentThreadSize_ > initThreadSize_) {
          // cached 模式中，超过初始数量的线程不会永久等待。
          // 如果空闲时间达到上限还没有任务，它会退出并回收自己。
          auto status = notEmpty_.wait_for(lock, threadMaxIdleTime_);
          if (status == std::cv_status::timeout && taskQue_.empty() &&
              currentThreadSize_ > initThreadSize_) {
            // 退出前必须同步修正两个计数：
            // idleThreadSize_ 表示少了一个空闲线程；
            // currentThreadSize_ 表示线程池总线程数减少。
            --idleThreadSize_;
            finishThread();
            return;
          }
        } else {
          // fixed 模式，或者 cached 模式中的初始线程，需要一直等待任务。
          notEmpty_.wait(lock);
        }
      }

      // 线程即将取任务或退出等待区，不再算作空闲。
      --idleThreadSize_;

      if (!isPoolRunning_ && taskQue_.empty()) {
        finishThread();
        return;
      }

      task = std::move(taskQue_.front());
      taskQue_.pop();
    }

    // 取走一个任务后，任务队列有了空位，可以唤醒可能阻塞的 submit()。
    notFull_.notify_one();

    // 真正执行用户任务。
    // 如果用户任务抛异常，packaged_task 会捕获异常并保存到 future 中。
    task();
  }
}
