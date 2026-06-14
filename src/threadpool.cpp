#include "threadpool.h"

#include <exception>

namespace {
constexpr std::size_t TASK_MAX_THRESHOLD = 1024;
}

// ResultState 是 Result 和工作线程之间共享的状态。
// 它内部用条件变量实现“一方等待结果，另一方写入结果后唤醒”。
class ResultState {
 public:
  void setValue(Any value) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (ready_) {
        return;
      }
      value_ = std::move(value);
      ready_ = true;
    }

    // 解锁后再通知，减少被唤醒线程马上抢锁失败的概率。
    cond_.notify_all();
  }

  void setException(std::exception_ptr exception) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      if (ready_) {
        return;
      }
      exception_ = exception;
      ready_ = true;
    }
    cond_.notify_all();
  }

  Any get() {
    std::unique_lock<std::mutex> lock(mtx_);

    // 如果任务还没执行完，用户线程会阻塞在这里。
    // wait 的谓词可以防止“虚假唤醒”导致提前往下走。
    cond_.wait(lock, [this] { return ready_; });

    // 任务线程里抛出的异常，在用户调用 get() 的线程里重新抛出。
    if (exception_) {
      std::rethrow_exception(exception_);
    }

    // 当前 Any 是只移动的，所以同一个 Result 只能 get() 一次。
    if (consumed_) {
      throw std::runtime_error("Result has already been consumed");
    }

    consumed_ = true;
    return std::move(value_);
  }

 private:
  std::mutex mtx_;
  std::condition_variable cond_;
  bool ready_ = false;
  bool consumed_ = false;
  Any value_;
  std::exception_ptr exception_;
};

// 默认构造出来的是无效 Result，主要用于占位。
Result::Result() : isValid_(false) {}

Result::Result(std::shared_ptr<ResultState> state, bool isValid)
    : state_(std::move(state)), isValid_(isValid) {}

Any Result::get() {
  if (!isValid_ || !state_) {
    throw std::runtime_error("Invalid Result");
  }

  // 真正的等待逻辑在共享状态里完成。
  return state_->get();
}

bool Result::valid() const noexcept {
  return isValid_ && state_ != nullptr;
}

Thread::Thread(ThreadFunc func) : func_(std::move(func)) {}

Thread::~Thread() {
  join();
}

void Thread::start() {
  if (thread_.joinable()) {
    throw std::runtime_error("Thread has already been started");
  }

  // 线程启动后会执行传进来的函数，这里就是 ThreadPool::threadFunc。
  thread_ = std::thread(func_);
}

void Thread::join() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

ThreadPool::ThreadPool()
    : initThreadSize_(0),
      taskQueMaxThreshold_(TASK_MAX_THRESHOLD),
      poolMode_(ThreadPoolMode::MODE_FIXED),
      isPoolRunning_(false) {}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lock(taskQueMtx_);

    // 通知所有工作线程：线程池要结束了。
    // 如果队列里还有任务，工作线程会先把剩余任务取完；队列空了再退出。
    isPoolRunning_ = false;
  }

  // 唤醒可能阻塞在“队列空”或“队列满”条件上的线程，让它们重新检查退出条件。
  notEmpty_.notify_all();
  notFull_.notify_all();

  // 等待工作线程全部退出，防止线程池对象销毁后线程还在访问 this。
  for (auto& thread : threads_) {
    thread->join();
  }
}

void ThreadPool::setMode(ThreadPoolMode poolMode) {
  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_) {
    throw std::runtime_error("Cannot change thread pool mode after start");
  }
  if (poolMode == ThreadPoolMode::MODE_CACHED) {
    throw std::logic_error("MODE_CACHED is not implemented in this version");
  }
  poolMode_ = poolMode;
}

void ThreadPool::setTaskQueMaxThreshHold(std::size_t threshold) {
  if (threshold == 0) {
    throw std::invalid_argument("Task queue threshold must be greater than 0");
  }

  std::lock_guard<std::mutex> lock(taskQueMtx_);
  if (isPoolRunning_) {
    throw std::runtime_error("Cannot change task queue threshold after start");
  }
  taskQueMaxThreshold_ = threshold;
}

void ThreadPool::start(std::size_t initThreadSize) {
  if (initThreadSize == 0) {
    throw std::invalid_argument("Initial thread size must be greater than 0");
  }

  {
    std::lock_guard<std::mutex> lock(taskQueMtx_);
    if (isPoolRunning_) {
      throw std::runtime_error("Thread pool has already been started");
    }
    initThreadSize_ = initThreadSize;

    // 必须先把运行状态设为 true，再启动工作线程。
    // 否则工作线程一启动就可能看到 false 并退出。
    isPoolRunning_ = true;
  }

  threads_.reserve(initThreadSize_);
  for (std::size_t i = 0; i < initThreadSize_; ++i) {
    threads_.push_back(std::make_unique<Thread>(
        std::bind(&ThreadPool::threadFunc, this)));
  }

  for (auto& thread : threads_) {
    thread->start();
  }
}

Result ThreadPool::submitTask(std::shared_ptr<Task> task) {
  // 先创建结果状态，再把“任务 + 结果状态”一起放入队列。
  // 这是修复原代码竞态的关键：工作线程取到任务时，一定能找到对应的结果对象。
  auto resultState = std::make_shared<ResultState>();

  if (!task) {
    resultState->setException(
        std::make_exception_ptr(std::invalid_argument("Task must not be null")));
    return Result(resultState);
  }

  {
    std::unique_lock<std::mutex> lock(taskQueMtx_);
    if (!isPoolRunning_) {
      resultState->setException(
          std::make_exception_ptr(std::runtime_error("Thread pool is not running")));
      return Result(resultState);
    }

    // 队列满时，提交任务的线程在这里等待。
    // 析构时 isPoolRunning_ 会变 false，这样等待中的 submitTask 也能醒来退出。
    notFull_.wait(lock, [this] {
      return taskQue_.size() < taskQueMaxThreshold_ || !isPoolRunning_;
    });

    if (!isPoolRunning_) {
      resultState->setException(
          std::make_exception_ptr(std::runtime_error("Thread pool has stopped")));
      return Result(resultState);
    }

    taskQue_.push(TaskItem{std::move(task), resultState});
  }

  // 通知一个工作线程：队列里有任务了。
  notEmpty_.notify_one();
  return Result(resultState);
}

void ThreadPool::threadFunc() {
  for (;;) {
    TaskItem item;

    {
      std::unique_lock<std::mutex> lock(taskQueMtx_);

      // 队列为空时，工作线程在这里睡眠，避免空转占用 CPU。
      notEmpty_.wait(lock, [this] {
        return !taskQue_.empty() || !isPoolRunning_;
      });

      // 线程池停止且没有剩余任务时，工作线程退出循环。
      if (!isPoolRunning_ && taskQue_.empty()) {
        return;
      }

      item = std::move(taskQue_.front());
      taskQue_.pop();
    }

    // 已经取走一个任务，队列有空位了，可以唤醒提交任务的一方。
    notFull_.notify_one();

    try {
      // 真正执行用户自定义的任务逻辑。
      item.result->setValue(item.task->run());
    } catch (...) {
      // 不让工作线程因为用户任务异常而崩掉；
      // 异常会保存到 ResultState，等用户调用 get() 时再抛出。
      item.result->setException(std::current_exception());
    }
  }
}
