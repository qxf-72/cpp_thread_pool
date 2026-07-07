#include "threadpool.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// 测试失败时抛出统一异常，main() 里集中捕获并打印失败信息。
class TestFailure : public std::runtime_error {
public:
  explicit TestFailure(const std::string &message)
      : std::runtime_error(message) {}
};

// 极简断言工具：避免引入第三方测试框架，让项目仍然保持轻量。
void check(bool condition, const char *expr, const char *file, int line) {
  if (condition) {
    return;
  }

  throw TestFailure(std::string(file) + ":" + std::to_string(line) +
                    ": check failed: " + expr);
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

// 检查表达式是否抛出异常，并验证异常信息里包含预期片段。
template <typename Func>
void checkThrowsWithMessage(Func &&func, const std::string &messagePart,
                            const char *expr, const char *file, int line) {
  try {
    func();
  } catch (const std::exception &e) {
    if (contains(e.what(), messagePart)) {
      return;
    }

    throw TestFailure(std::string(file) + ":" + std::to_string(line) +
                      ": unexpected exception message from " + expr + ": " +
                      e.what());
  }

  throw TestFailure(std::string(file) + ":" + std::to_string(line) +
                    ": expected exception from " + expr);
}

#define CHECK_THROWS_WITH(expr, messagePart)                                  \
  checkThrowsWithMessage([&] { (void)(expr); }, (messagePart), #expr,          \
                         __FILE__, __LINE__)

template <typename Predicate>
bool waitUntil(Predicate &&predicate, std::chrono::milliseconds timeout) {
  // 并发测试不能依赖固定 sleep，轮询等待能降低机器负载差异带来的误判。
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

void testSubmitReturnsValues() {
  // 验证普通任务和带参数任务都能通过 future 拿到正确返回值。
  ThreadPool pool;
  pool.start(2);

  auto answer = pool.submit([] { return 42; });
  auto sum = pool.submit([](int a, int b) { return a + b; }, 10, 20);

  CHECK(answer.get() == 42);
  CHECK(sum.get() == 30);

  pool.shutdown();
}

void testVoidTasksRun() {
  // 验证 void 任务会被真正执行，future<void>::get() 能等待任务完成。
  ThreadPool pool;
  pool.start(2);

  std::atomic<int> counter{0};
  std::vector<std::future<void>> futures;

  for (int i = 0; i < 8; ++i) {
    futures.emplace_back(pool.submit([&counter] { ++counter; }));
  }

  for (auto &future : futures) {
    future.get();
  }

  CHECK(counter.load() == 8);
  pool.shutdown();
}

void testTaskExceptionPropagatesToFuture() {
  // 用户任务抛出的异常应由 packaged_task 保存，并在 future.get() 时重新抛出。
  ThreadPool pool;
  pool.start(1);

  auto result = pool.submit([]() -> int {
    throw std::runtime_error("task failed");
  });

  CHECK_THROWS_WITH(result.get(), "task failed");
  pool.shutdown();
}

void testSubmitBeforeStartFails() {
  // 未调用 start() 前，线程池不接收任务。
  ThreadPool pool;

  CHECK_THROWS_WITH(pool.submit([] { return 1; }), "not running");
}

void testConfigurationAfterStartFails() {
  // 配置项只能在启动前修改，避免运行期改变线程池核心约束。
  ThreadPool pool;
  pool.start(1);

  CHECK_THROWS_WITH(pool.setMode(ThreadPoolMode::MODE_CACHED), "after start");
  CHECK_THROWS_WITH(pool.setTaskQueMaxThreshold(8), "after start");
  CHECK_THROWS_WITH(pool.setThreadSizeThreshold(8), "after start");
  CHECK_THROWS_WITH(pool.setThreadMaxIdleTime(std::chrono::seconds(2)),
                    "after start");

  pool.shutdown();
}

void testShutdownRejectsNewTasksAndWaitsQueuedTasks() {
  // shutdown() 应停止接收新任务，但已经提交的任务仍要执行完成。
  ThreadPool pool;
  pool.start(1);

  std::atomic<int> finishedTasks{0};

  auto first = pool.submit([&finishedTasks] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ++finishedTasks;
    return 1;
  });

  auto second = pool.submit([&finishedTasks] {
    ++finishedTasks;
    return 2;
  });

  pool.shutdown();

  CHECK(first.get() == 1);
  CHECK(second.get() == 2);
  CHECK(finishedTasks.load() == 2);
  CHECK(pool.currentThreadSize() == 0);
  CHECK(pool.idleThreadSize() == 0);
  CHECK_THROWS_WITH(pool.submit([] { return 3; }), "not running");
}

void testShutdownFromWorkerTaskFails() {
  // shutdown() 需要由线程池外部调用；任务内部调用会破坏对象生命周期。
  ThreadPool pool;
  pool.start(1);

  auto result = pool.submit([&pool] { pool.shutdown(); });

  CHECK_THROWS_WITH(result.get(), "worker thread");

  // 拒绝 worker 内部 shutdown 后，线程池应仍处于可用状态。
  auto stillWorks = pool.submit([] { return 9; });
  CHECK(stillWorks.get() == 9);

  pool.shutdown();
}

void testCanRestartAfterShutdown() {
  // shutdown() 会完整回收线程；之后允许同一个对象再次 start()。
  ThreadPool pool;
  pool.start(1);

  auto first = pool.submit([] { return 1; });
  CHECK(first.get() == 1);
  pool.shutdown();
  CHECK(pool.currentThreadSize() == 0);
  CHECK(pool.idleThreadSize() == 0);

  pool.start(2);
  auto second = pool.submit([] { return 2; });
  CHECK(second.get() == 2);
  CHECK(pool.currentThreadSize() == 2);

  pool.shutdown();
}

void testCachedModeRejectsInitialSizeAboveThreshold() {
  // cached 模式下，初始线程数不能超过最大线程数配置。
  ThreadPool pool;
  pool.setMode(ThreadPoolMode::MODE_CACHED);
  pool.setThreadSizeThreshold(1);

  CHECK_THROWS_WITH(pool.start(2), "threshold");
  CHECK(pool.currentThreadSize() == 0);

  // 启动失败后对象应回到可配置、可重新启动的干净状态。
  pool.setThreadSizeThreshold(2);
  pool.start(1);
  auto result = pool.submit([] { return 6; });
  CHECK(result.get() == 6);
  pool.shutdown();
}

void testDestructorShutsDownFromExternalThread() {
  // 外部线程销毁 ThreadPool 时，析构函数应自动走关闭流程。
  std::future<int> result;

  {
    ThreadPool pool;
    pool.start(1);
    result = pool.submit([] { return 11; });
    CHECK(result.get() == 11);
  }
}

void testQueueFullSubmitTimesOut() {
  // 用一个阻塞任务占住唯一工作线程，再填满队列，验证第三次提交会超时失败。
  ThreadPool pool;
  pool.setTaskQueMaxThreshold(1);
  pool.start(1);

  std::promise<void> workerStarted;
  auto workerStartedFuture = workerStarted.get_future();

  // shared_future 可以被复制进任务闭包，同时在测试线程中控制释放时机。
  std::promise<void> releaseWorker;
  auto releaseWorkerFuture = releaseWorker.get_future().share();
  bool workerReleased = false;

  // 异常路径也要释放阻塞任务，避免测试失败时 shutdown() 永久等待。
  auto release = [&] {
    if (!workerReleased) {
      releaseWorker.set_value();
      workerReleased = true;
    }
  };

  auto blocker = pool.submit([&workerStarted, releaseWorkerFuture] {
    workerStarted.set_value();
    releaseWorkerFuture.wait();
  });

  try {
    CHECK(workerStartedFuture.wait_for(std::chrono::seconds(1)) ==
          std::future_status::ready);

    auto queued = pool.submit([] { return 7; });

    const auto begin = Clock::now();
    CHECK_THROWS_WITH(pool.submit([] { return 8; }), "timed out");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - begin);

    CHECK(elapsed >= std::chrono::milliseconds(900));
    CHECK(elapsed < std::chrono::milliseconds(2500));

    release();
    blocker.get();
    CHECK(queued.get() == 7);
    pool.shutdown();
  } catch (...) {
    release();
    throw;
  }
}

void testCachedModeScalesAndReclaimsIdleWorkers() {
  // 验证 cached 模式在任务积压时会扩容，并在空闲超时后回收到初始线程数。
  ThreadPool pool;
  pool.setMode(ThreadPoolMode::MODE_CACHED);
  pool.setThreadSizeThreshold(4);
  pool.setThreadMaxIdleTime(std::chrono::seconds(1));
  pool.start(1);

  std::promise<void> releaseTasks;
  auto releaseFuture = releaseTasks.get_future().share();
  bool tasksReleased = false;

  // 多个任务共用同一个释放信号，便于制造稳定的任务积压。
  auto release = [&] {
    if (!tasksReleased) {
      releaseTasks.set_value();
      tasksReleased = true;
    }
  };

  std::vector<std::future<int>> futures;

  try {
    for (int i = 0; i < 8; ++i) {
      futures.emplace_back(pool.submit([releaseFuture] {
        releaseFuture.wait();
        return 1;
      }));
    }

    CHECK(waitUntil([&pool] { return pool.currentThreadSize() > 1; },
                    std::chrono::seconds(2)));
    CHECK(pool.currentThreadSize() <= 4);

    release();
    for (auto &future : futures) {
      CHECK(future.get() == 1);
    }

    CHECK(waitUntil([&pool] { return pool.currentThreadSize() == 1; },
                    std::chrono::seconds(3)));

    pool.shutdown();
  } catch (...) {
    release();
    throw;
  }
}

struct TestCase {
  const char *name;
  void (*func)();
};

// 手写测试表，main() 逐个执行；新增测试时只需要把函数加入这里。
const TestCase TESTS[] = {
    {"submit returns values", testSubmitReturnsValues},
    {"void tasks run", testVoidTasksRun},
    {"task exception propagates to future", testTaskExceptionPropagatesToFuture},
    {"submit before start fails", testSubmitBeforeStartFails},
    {"configuration after start fails", testConfigurationAfterStartFails},
    {"shutdown rejects new tasks and waits queued tasks",
     testShutdownRejectsNewTasksAndWaitsQueuedTasks},
    {"shutdown from worker task fails", testShutdownFromWorkerTaskFails},
    {"can restart after shutdown", testCanRestartAfterShutdown},
    {"cached mode rejects initial size above threshold",
     testCachedModeRejectsInitialSizeAboveThreshold},
    {"destructor shuts down from external thread",
     testDestructorShutsDownFromExternalThread},
    {"queue full submit times out", testQueueFullSubmitTimesOut},
    {"cached mode scales and reclaims idle workers",
     testCachedModeScalesAndReclaimsIdleWorkers},
};

} // namespace

int main() {
  int failed = 0;

  for (const auto &test : TESTS) {
    try {
      test.func();
      std::cout << "[PASS] " << test.name << '\n';
    } catch (const std::exception &e) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": " << e.what() << '\n';
    } catch (...) {
      ++failed;
      std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
    }
  }

  if (failed == 0) {
    std::cout << "All thread pool tests passed.\n";
    return 0;
  }

  std::cerr << failed << " test(s) failed.\n";
  return 1;
}
