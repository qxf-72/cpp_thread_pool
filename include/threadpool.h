#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Any 是一个“类型擦除”容器：
// 线程池不知道用户任务会返回 int、string 还是自定义对象，所以用 Any 暂存任意返回值。
// 注意：这个 Any 只能移动，不能拷贝；get() 取出结果后，结果就被消费掉了。
class Any {
 public:
  Any() = default;
  ~Any() = default;

  Any(const Any&) = delete;
  Any& operator=(const Any&) = delete;

  Any(Any&&) noexcept = default;
  Any& operator=(Any&&) noexcept = default;

  template <typename T,
            typename Decayed = std::decay_t<T>,
            typename = std::enable_if_t<!std::is_same_v<Decayed, Any>>>
  Any(T&& data) : base_(std::make_unique<Derive<Decayed>>(std::forward<T>(data))) {}

  // 按指定类型取出数据。
  // 例如：int value = result.get().cast_<int>();
  // 如果类型写错，会抛出异常，避免静默得到错误结果。
  template <typename T>
  T cast_() {
    using Decayed = std::decay_t<T>;
    auto* pd = dynamic_cast<Derive<Decayed>*>(base_.get());
    if (pd == nullptr) {
      throw std::runtime_error("Any cast failed: type does not match");
    }
    return std::move(pd->data_);
  }

  bool empty() const noexcept {
    return base_ == nullptr;
  }

 private:
  class Base {
   public:
    virtual ~Base() = default;
  };

  template <typename T>
  class Derive : public Base {
   public:
    template <typename U>
    explicit Derive(U&& data) : data_(std::forward<U>(data)) {}

    T data_;
  };

  std::unique_ptr<Base> base_;
};

class ResultState;

// Result 是 submitTask() 返回给用户的“结果句柄”。
// 用户线程调用 get()，会一直等到工作线程把任务结果写入 ResultState。
class Result {
 public:
  Result();

  Any get();
  bool valid() const noexcept;

 private:
  friend class ThreadPool;

  explicit Result(std::shared_ptr<ResultState> state, bool isValid = true);

  std::shared_ptr<ResultState> state_;
  bool isValid_;
};

// 用户任务基类。
// 自定义任务时继承 Task，并重写 run()；run() 的返回值就是 Result::get() 拿到的值。
class Task {
 public:
  Task() = default;
  virtual ~Task() = default;

  virtual Any run() = 0;
};

enum class ThreadPoolMode {
  MODE_FIXED,
  MODE_CACHED
};

// 对 std::thread 做一层简单封装。
// 这里保存真正的线程对象，析构前会 join，避免后台线程继续访问已经销毁的线程池。
class Thread {
 public:
  using ThreadFunc = std::function<void()>;

  explicit Thread(ThreadFunc func);
  ~Thread();

  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  void start();
  void join();

 private:
  ThreadFunc func_;
  std::thread thread_;
};

// 固定线程数的线程池。
// 使用流程：
//   ThreadPool pool;
//   pool.start(4);
//   Result r = pool.submitTask(task);
//   auto value = r.get().cast_<int>();
class ThreadPool {
 private:
  template <typename Func, typename... BoundArgs>
  class FunctionTask;

 public:
  ThreadPool();
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  void start(std::size_t initThreadSize = 4);
  void setMode(ThreadPoolMode poolMode);
  void setTaskQueMaxThreshHold(std::size_t threshold);

  // 更方便的任务提交接口。
  // 用户不需要再手写 Task 子类，可以直接提交 lambda、普通函数、函数对象等。
  //
  // 用法示例：
  //   auto r1 = pool.submit([] { return 42; });
  //   int value = r1.get().cast_<int>();
  //
  //   auto r2 = pool.submit([](int a, int b) { return a + b; }, 10, 20);
  //   int sum = r2.get().cast_<int>();
  template <typename F, typename... Args>
  Result submit(F&& func, Args&&... args) {
    using TaskType = FunctionTask<std::decay_t<F>, std::decay_t<Args>...>;

    return submitTask(std::make_shared<TaskType>(
        std::forward<F>(func), std::forward<Args>(args)...));
  }

  // 保留原来的底层接口：如果你想自己定义 Task 子类，也仍然可以使用它。
  Result submitTask(std::shared_ptr<Task> task);

 private:
  // submit() 会把任意可调用对象包装成 FunctionTask，
  // 这样线程池内部仍然只需要处理统一的 Task*。
  template <typename Func, typename... BoundArgs>
  class FunctionTask : public Task {
   public:
    template <typename F, typename... Args>
    FunctionTask(F&& func, Args&&... args)
        : func_(std::forward<F>(func)), args_(std::forward<Args>(args)...) {}

    Any run() override {
      return runImpl(std::index_sequence_for<BoundArgs...>{});
    }

   private:
    using ReturnType = std::invoke_result_t<Func&, BoundArgs&&...>;

    template <std::size_t... I>
    Any runImpl(std::index_sequence<I...>) {
      if constexpr (std::is_void_v<ReturnType>) {
        std::invoke(func_, std::move(std::get<I>(args_))...);
        return Any{};
      } else {
        return Any(std::invoke(func_, std::move(std::get<I>(args_))...));
      }
    }

    Func func_;
    std::tuple<BoundArgs...> args_;
  };

  // 队列里不能只放 Task，还要同时放它对应的结果状态。
  // 这样工作线程执行完任务后，能准确唤醒提交该任务的用户线程。
  struct TaskItem {
    std::shared_ptr<Task> task;
    std::shared_ptr<ResultState> result;
  };

  void threadFunc();

  std::vector<std::unique_ptr<Thread>> threads_;
  std::queue<TaskItem> taskQue_;

  std::size_t initThreadSize_;
  std::size_t taskQueMaxThreshold_;

  std::mutex taskQueMtx_;
  std::condition_variable notFull_;
  std::condition_variable notEmpty_;

  ThreadPoolMode poolMode_;

  // 受 taskQueMtx_ 保护。
  // true 表示线程池正在接收任务；false 表示析构或停止中，工作线程应该退出。
  bool isPoolRunning_;
};

#endif
