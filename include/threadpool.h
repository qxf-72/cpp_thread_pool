#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

enum class ThreadPoolMode { MODE_FIXED, MODE_CACHED };

class ThreadPool {
public:
  ThreadPool();
  ~ThreadPool() noexcept;

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  // 启动线程池。所有配置函数都应在 start() 之前调用。
  void start(std::size_t initThreadSize = 4);

  // 设置线程池模式：固定线程数或 cached 动态扩容。
  void setMode(ThreadPoolMode poolMode);

  // 设置任务队列容量上限。
  void setTaskQueMaxThreshold(std::size_t threshold);

  // 设置队列满时 submit() 等待空位的最长时间。
  void setSubmitTimeout(std::chrono::milliseconds timeout);

  // 设置 cached 模式下允许创建的最大线程数。
  void setThreadSizeThreshold(std::size_t threshold);

  // 设置 cached 模式下多余线程的最大空闲时间。
  void setThreadMaxIdleTime(std::chrono::seconds idleTime);

  // 显式关闭线程池：
  // 1. 停止接收新任务；
  // 2. 继续执行已经进入队列的任务；
  // 3. 等待工作线程退出并回收资源。
  void shutdown();

  // 这两个接口主要用于观察 cached 模式是否发生扩容和回收。
  std::size_t currentThreadSize() const;
  std::size_t idleThreadSize() const;

  template <typename F, typename... Args>
  auto submit(F &&func, Args &&...args) -> std::future<
      std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
    using ReturnType =
        std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    // 任务会持有函数和参数；需要引用语义时由调用方显式传入 std::ref。
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<F>(func),
         args = std::make_tuple(
             std::forward<Args>(args)...)]() mutable -> ReturnType {
          if constexpr (std::is_void_v<ReturnType>) {
            std::apply(std::move(func), std::move(args));
          } else {
            return std::apply(std::move(func), std::move(args));
          }
        });

    std::future<ReturnType> result = task->get_future();

    {
      std::unique_lock<std::mutex> lock(taskQueMtx_);
      if (!isPoolRunning_) {
        throw std::runtime_error("Thread pool is not running");
      }

      const auto submitDeadline =
          std::chrono::steady_clock::now() + submitTimeout_;
      auto waitForQueueSlot = [&] {
        if (!notFull_.wait_until(lock, submitDeadline, [this] {
              return taskQue_.size() < taskQueMaxThreshold_ || !isPoolRunning_;
            })) {
          throw std::runtime_error("Task queue is full; submit timed out");
        }

        // wait_until 会释放锁；被 shutdown() 唤醒后不能继续入队。
        if (!isPoolRunning_ || isShuttingDown_) {
          throw std::runtime_error(
              "Thread pool has stopped or is shutting down");
        }
      };

      if (poolMode_ == ThreadPoolMode::MODE_CACHED &&
          taskQue_.size() >= taskQueMaxThreshold_ &&
          currentThreadSize_ < threadSizeThreshold_) {
        reapFinishedThreadsLocked(lock);
        if (!isPoolRunning_ || isShuttingDown_) {
          throw std::runtime_error(
              "Thread pool has stopped or is shutting down");
        }
        if (currentThreadSize_ < threadSizeThreshold_) {
          try {
            addThreadLocked();
          } catch (...) {
            // 创建线程失败时继续走提交超时等待逻辑，由已有线程尽量消费队列。
          }
        }
      }

      waitForQueueSlot();

      // 在入队前回收线程。避免：任务已经入队，函数却抛异常。
      reapFinishedThreadsLocked(lock);
      // reapFinishedThreadsLocked() 可能在 join 时临时解锁；重新确认容量，
      // 避免其他提交线程趁机填满队列后仍继续入队。
      waitForQueueSlot();

      taskQue_.emplace([task] { (*task)(); });

      if (poolMode_ == ThreadPoolMode::MODE_CACHED) {
        if (taskQue_.size() > idleThreadSize_ &&
            currentThreadSize_ < threadSizeThreshold_) {
          try {
            addThreadLocked();
          } catch (...) {
            // 扩容失败不影响已入队任务，已有工作线程仍会继续消费队列。
          }
        }
      }
    }

    notEmpty_.notify_one();
    return result;
  }

private:
  struct WorkerState {
    // 由 taskQueMtx_ 保护；true 表示线程函数已经退出。
    bool finished = false;
  };

  struct Worker {
    std::thread thread;
    std::shared_ptr<WorkerState> state;
  };

  // 调用方必须持有 taskQueMtx_。
  void addThreadLocked();

  // 析构函数和 public shutdown() 共用的关闭实现。
  void shutdownImpl(bool rejectWorkerThread);

  // 调用方必须持有 taskQueMtx_；join 前会临时解锁。
  void reapFinishedThreadsLocked(std::unique_lock<std::mutex> &lock);
  void threadFunc(std::shared_ptr<WorkerState> state);

  static thread_local const ThreadPool *currentWorkerPool_;

  std::vector<Worker> threads_;
  std::queue<std::function<void()>> taskQue_;

  // 初始线程数，也是 cached 模式回收线程后的保底线程数。
  std::size_t initThreadSize_;

  std::size_t taskQueMaxThreshold_;

  std::chrono::milliseconds submitTimeout_;

  std::size_t threadSizeThreshold_;

  std::size_t currentThreadSize_;

  std::size_t idleThreadSize_;

  std::chrono::seconds threadMaxIdleTime_;

  // 以下成员状态都由 taskQueMtx_ 保护。
  mutable std::mutex taskQueMtx_;

  std::condition_variable notFull_;

  std::condition_variable notEmpty_;

  ThreadPoolMode poolMode_;

  bool isPoolRunning_;

  bool isShuttingDown_;
};

#endif
