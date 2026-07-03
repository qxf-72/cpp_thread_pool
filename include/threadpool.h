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

// 线程池模式：
// - MODE_FIXED：启动时创建固定数量线程，之后线程数不变。
// - MODE_CACHED：任务压力变大时自动增加线程，空闲一段时间后回收多余线程。
enum class ThreadPoolMode {
  MODE_FIXED, // 固定数量线程
  MODE_CACHED // 任务多时动态扩容，空闲超时后回收线程
};

// ThreadPool 对外只暴露 submit() + future 接口：
// 用户提交任意可调用对象，线程池内部把它包装成 packaged_task，
// 再把 packaged_task 放进任务队列，最终通过 std::future<T> 返回结果。
class ThreadPool {
public:
  ThreadPool();
  ~ThreadPool();

  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  // 启动线程池。所有配置函数都应在 start() 之前调用。
  void start(std::size_t initThreadSize = 4);

  // 设置线程池模式：固定线程数或 cached 动态扩容。
  void setMode(ThreadPoolMode poolMode);

  // 设置任务队列容量上限。
  // 队列满时 submit() 最多等待 1 秒，仍无空位则抛出异常表示提交失败。
  void setTaskQueMaxThreshold(std::size_t threshold);

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
    // 根据用户传入的函数和参数，推导任务真正的返回类型。
    // 例如 submit([] { return 1; }) 的 ReturnType 就是 int。
    using ReturnType =
        std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    // packaged_task 负责两件事：
    // 1. 在工作线程中执行用户任务；
    // 2. 自动把返回值或异常保存到对应的 future 中。
    //
    // 这里把 func 和 args 都移动/拷贝进 lambda，是为了保证 submit() 返回后，
    // 工作线程执行任务时仍然拥有完整的函数和参数对象。
    auto task = std::make_shared<std::packaged_task<ReturnType()>>(
        [func = std::forward<F>(func),
         args = std::make_tuple(
             std::forward<Args>(args)...)]() mutable -> ReturnType {
          if constexpr (std::is_void_v<ReturnType>) {
            // void 返回值任务不需要 return，只需要执行完即可让 future 变为
            // ready。
            std::apply(std::move(func), std::move(args));
          } else {
            return std::apply(std::move(func), std::move(args));
          }
        });

    // future 必须在 packaged_task 被放进队列之前取出来。
    // 用户拿到这个 future 后，就可以调用 get() 等待任务结果。
    std::future<ReturnType> result = task->get_future();

    {
      std::unique_lock<std::mutex> lock(taskQueMtx_);
      if (!isPoolRunning_) {
        throw std::runtime_error("Thread pool is not running");
      }

      const auto submitDeadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(1);

      // 如果队列已满且 cached 模式还能扩容，先尝试补一个工作线程，
      // 这样当前提交不必无意义地等待已有线程慢慢腾出队列空间。
      if (poolMode_ == ThreadPoolMode::MODE_CACHED &&
          taskQue_.size() >= taskQueMaxThreshold_ &&
          currentThreadSize_ < threadSizeThreshold_) {
        reapFinishedThreadsLocked(lock);
        if (currentThreadSize_ < threadSizeThreshold_) {
          try {
            addThreadLocked();
          } catch (...) {
            // 创建线程失败时继续走 1 秒等待逻辑，由已有线程尽量消费队列。
          }
        }
      }

      // 提交失败策略：队列满时最多阻塞 1 秒，超时就拒绝本次任务。
      if (!notFull_.wait_until(lock, submitDeadline, [this] {
            return taskQue_.size() < taskQueMaxThreshold_ || !isPoolRunning_;
          })) {
        throw std::runtime_error("Task queue is full; submit timed out");
      }

      // wait_until可能释放了锁，必须再次判断线程池状态。
      if (!isPoolRunning_) {
        throw std::runtime_error("Thread pool has stopped");
      }

      // 队列里统一保存 std::function<void()>。
      // packaged_task 本身不可拷贝，所以这里用 shared_ptr 包一层，再捕获进
      // lambda。
      taskQue_.emplace([task] { (*task)(); });

      // cached 模式扩容条件：
      // 1. 任务数量已经超过空闲线程数量，说明现有空闲线程不够用了；
      // 2. 当前线程数还没达到上限。
      if (poolMode_ == ThreadPoolMode::MODE_CACHED) {
        reapFinishedThreadsLocked(lock);
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
    // 工作线程退出前会置为 true，线程池随后 join 并移除对应线程对象。
    bool finished = false;
  };

  struct Worker {
    // std::thread 不可拷贝，因此 Worker 只在 vector 中移动。
    std::thread thread;
    std::shared_ptr<WorkerState> state;
  };

  // 调用这个函数时，外层必须已经持有 taskQueMtx_。
  // 这样可以保证 currentThreadSize_ 和 threads_ 的修改是同步的。
  void addThreadLocked();

  // 回收已经自然退出的 cached 工作线程。
  // 调用方传入已上锁的 unique_lock；函数会在 join 时临时解锁。
  void reapFinishedThreadsLocked(std::unique_lock<std::mutex> &lock);
  void threadFunc(std::shared_ptr<WorkerState> state);

  std::vector<Worker> threads_;
  std::queue<std::function<void()>> taskQue_;

  // initThreadSize_：初始线程数，也是 cached 模式回收线程后的保底线程数。
  std::size_t initThreadSize_;

  // 任务队列容量上限。队列满时，submit() 最多等待 1 秒。
  std::size_t taskQueMaxThreshold_;

  // cached 模式下的最大线程数。
  std::size_t threadSizeThreshold_;

  // 当前活跃线程总数，包括正在执行任务和正在空闲等待的线程。
  std::size_t currentThreadSize_;

  // 当前空闲线程数，用于判断是否需要扩容。
  std::size_t idleThreadSize_;

  // cached 模式下，超过初始数量的线程空闲多久后退出。
  std::chrono::seconds threadMaxIdleTime_;

  // 以下状态都由 taskQueMtx_ 保护。
  mutable std::mutex taskQueMtx_;

  // notFull_：队列满时 submit() 等待，工作线程取走任务后唤醒。
  std::condition_variable notFull_;

  // notEmpty_：队列空时工作线程等待，submit() 放入任务后唤醒。
  std::condition_variable notEmpty_;

  ThreadPoolMode poolMode_;

  // true 表示线程池正在接收任务；false 表示正在关闭或尚未启动。
  bool isPoolRunning_;

  // 防止多个线程同时执行关闭流程。
  bool isShuttingDown_;
};

#endif
