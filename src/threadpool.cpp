#include "threadpool.h"

#include <algorithm>
#include <exception>

namespace {
// 默认任务队列容量。
constexpr std::size_t TASK_MAX_THRESHOLD = 1024;

// 队列满时，submit() 默认最多等待 1 秒。
constexpr auto SUBMIT_TIMEOUT = std::chrono::milliseconds(1000);

// cached 模式默认最大线程数。
constexpr std::size_t THREAD_SIZE_THRESHOLD = 16;

// cached 模式下，多出来的线程默认空闲 60 秒后退出。
constexpr auto THREAD_MAX_IDLE_TIME = std::chrono::seconds(60);
} // namespace

ThreadPool::ThreadPool()
    : initThreadSize_(0), taskQueMaxThreshold_(TASK_MAX_THRESHOLD),
      submitTimeout_(SUBMIT_TIMEOUT), threadSizeThreshold_(THREAD_SIZE_THRESHOLD),
      currentThreadSize_(0), idleThreadSize_(0),
      threadMaxIdleTime_(THREAD_MAX_IDLE_TIME),
      poolMode_(ThreadPoolMode::MODE_FIXED), isPoolRunning_(false),
      isShuttingDown_(false) {}

thread_local const ThreadPool *ThreadPool::currentWorkerPool_ = nullptr;

ThreadPool::~ThreadPool() noexcept {
  if (currentWorkerPool_ == this) {
    std::terminate();
  }

  try {
    shutdownImpl(false);
  } catch (...) {
    std::terminate();
  }
}

void ThreadPool::shutdown() { shutdownImpl(true); }

void ThreadPool::shutdownImpl(bool rejectWorkerThread) {
  if (currentWorkerPool_ == this) {
    if (rejectWorkerThread) {
      throw std::runtime_error("Cannot call shutdown from worker thread");
    }
    std::terminate();
  }

  std::vector<Worker> workers;

  {
    std::unique_lock<std::mutex> lock(taskQueMtx_);

    // shutdown() 允许重复调用；并发关闭时只让一个线程执行回收。
    if (isShuttingDown_) {
      // 保证shutdown()语义：只要返回，线程池就已经关闭。
      notEmpty_.wait(lock, [this] { return !isShuttingDown_; });
      return;
    }

    if (!isPoolRunning_ && threads_.empty()) {
      return;
    }

    // 先阻止新任务入队，队列中已有任务继续由 worker 消费。
    isShuttingDown_ = true;
    isPoolRunning_ = false;
  }

  // 唤醒 worker 和可能阻塞在 submit() 中的提交线程，让它们重新检查状态。
  notEmpty_.notify_all();
  notFull_.notify_all();

  {
    std::lock_guard<std::mutex> lock(taskQueMtx_);
    workers.swap(threads_);
  }

  // join 不能在持锁状态下做，否则 worker 退出时可能反向等待同一把锁。
  for (auto &worker : workers) {
    if (!worker.thread.joinable()) {
      continue;
    }
    worker.thread.join();
  }

  {
    std::lock_guard<std::mutex> lock(taskQueMtx_);
    currentThreadSize_ = 0;
    idleThreadSize_ = 0;
    isShuttingDown_ = false;
  }

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

void ThreadPool::setSubmitTimeout(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0) {
    throw std::invalid_argument("Submit timeout must be greater than 0");
  }

  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_ || isShuttingDown_) {
    throw std::runtime_error("Cannot change submit timeout after start");
  }
  submitTimeout_ = timeout;
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

  std::vector<Worker> workersToJoin;
  std::unique_lock<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_ || isShuttingDown_) {
    throw std::runtime_error("Thread pool has already been started");
  }

  const std::size_t previousInitThreadSize = initThreadSize_;
  initThreadSize_ = initThreadSize;

  try {
    if (poolMode_ == ThreadPoolMode::MODE_CACHED &&
        initThreadSize > threadSizeThreshold_) {
      throw std::invalid_argument(
          "Initial thread size cannot exceed thread size threshold");
    }

    threads_.reserve(std::max(threadSizeThreshold_, initThreadSize_));
    for (std::size_t i = 0; i < initThreadSize_; ++i) {
      addThreadLocked();
    }

    // 所有 worker 都创建成功后再进入 running 状态。
    isPoolRunning_ = true;
  } catch (...) {
    isPoolRunning_ = false;
    isShuttingDown_ = true;
    initThreadSize_ = previousInitThreadSize;
    workersToJoin.swap(threads_);

    lock.unlock();
    notEmpty_.notify_all();

    for (auto &worker : workersToJoin) {
      if (worker.thread.joinable()) {
        worker.thread.join();
      }
    }

    lock.lock();
    currentThreadSize_ = 0;
    idleThreadSize_ = 0;
    isShuttingDown_ = false;
    lock.unlock();

    notEmpty_.notify_all();
    throw;
  }
}

void ThreadPool::addThreadLocked() {
  // 调用方持有 taskQueMtx_；失败时必须回滚已修改的状态。
  threads_.emplace_back();
  bool counted = false;
  try {
    auto &worker = threads_.back();
    worker.state = std::make_shared<WorkerState>();
    ++currentThreadSize_;
    counted = true;

    auto state = worker.state;
    worker.thread = std::thread([this, state] { threadFunc(state); });
  } catch (...) {
    if (counted) {
      --currentThreadSize_;
    }
    threads_.pop_back();
    throw;
  }
}

void ThreadPool::reapFinishedThreadsLocked(std::unique_lock<std::mutex> &lock) {
  for (;;) {
    // join 期间会解锁，不能复用解锁前保存的迭代器继续遍历。
    auto it = std::find_if(threads_.begin(), threads_.end(),
                           [](const Worker &worker) {
                             return worker.state && worker.state->finished;
                           });
    if (it == threads_.end()) {
      return;
    }

    std::thread thread = std::move(it->thread);
    it = threads_.erase(it);

    // join 可能阻塞，必须放到锁外执行。
    lock.unlock();
    if (thread.joinable()) {
      thread.join();
    }
    lock.lock();
  }
}

void ThreadPool::threadFunc(std::shared_ptr<WorkerState> state) {
  struct WorkerPoolGuard {
    const ThreadPool *&slot;
    const ThreadPool *previous;

    ~WorkerPoolGuard() { slot = previous; }
  };

  WorkerPoolGuard guard{currentWorkerPool_, currentWorkerPool_};
  currentWorkerPool_ = this;

  // 只能在持有 taskQueMtx_ 时调用。
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

      ++idleThreadSize_;

      while (taskQue_.empty() && isPoolRunning_) {
        if (poolMode_ == ThreadPoolMode::MODE_CACHED &&
            currentThreadSize_ > initThreadSize_) {
          auto status = notEmpty_.wait_for(lock, threadMaxIdleTime_);
          if (status == std::cv_status::timeout && taskQue_.empty() &&
              currentThreadSize_ > initThreadSize_) {
            // 退出前必须同步修正计数并标记 finished，供后续 join 回收。
            --idleThreadSize_;
            finishThread();
            return;
          }
        } else {
          notEmpty_.wait(lock);
        }
      }

      --idleThreadSize_;

      if (!isPoolRunning_ && taskQue_.empty()) {
        finishThread();
        return;
      }

      task = std::move(taskQue_.front());
      taskQue_.pop();
    }

    notFull_.notify_one();

    task();
  }
}
