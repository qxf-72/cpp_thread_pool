#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "threadpool.h"

int add(int a, int b) { return a + b; }

int main() {
  ThreadPool pool;

  // 线程池的配置项需要在 start() 之前设置。
  // 使用 cached 模式：
  // 初始启动 2 个线程，任务多时最多扩容到 8 个线程。
  // 多出来的线程空闲超过 1 秒后会自动退出。
  pool.setMode(ThreadPoolMode::MODE_CACHED);
  pool.setThreadSizeThreshold(8);
  pool.setThreadMaxIdleTime(std::chrono::seconds(1));
  pool.start(2);

  auto r1 = pool.submit([]() -> int {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 42;
  });

  auto r2 = pool.submit(add, 10, 20);

  auto r3 =
      pool.submit([]() -> std::string { return std::string("hello future"); });

  std::vector<std::future<int>> results;
  for (int i = 0; i < 10; ++i) {
    // 一次性提交多个耗时任务，制造任务积压，观察 cached 模式扩容。
    // 如果队列满且 1 秒内没有空位，submit() 会抛出异常表示提交失败。
    results.emplace_back(pool.submit([i] {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      return i * i;
    }));
  }

  std::cout << "current threads: " << pool.currentThreadSize() << std::endl;
  std::cout << "r1: " << r1.get() << std::endl;
  std::cout << "r2: " << r2.get() << std::endl;
  std::cout << "r3: " << r3.get() << std::endl;

  for (auto &result : results) {
    std::cout << result.get() << ' ';
  }
  std::cout << std::endl;

  // 等待足够长时间，让 cached 模式中多余的空闲线程触发超时回收。
  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::cout << "current threads after idle timeout: "
            << pool.currentThreadSize() << std::endl;

  // 显式关闭会停止接收新任务，并等待已提交任务执行完成。
  // 即使不手动调用，ThreadPool 析构时也会自动执行同样的关闭流程。
  pool.shutdown();

  return 0;
}
