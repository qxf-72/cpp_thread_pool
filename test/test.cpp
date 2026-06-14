#include <iostream>
#include <memory>
#include <chrono>

#include "threadpool.h"

int main() {
  ThreadPool pool;
  pool.start(2);

  auto r1 = pool.submit([] {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 42; });

  auto r2 = pool.submit([](int a, int b) { return a + b; }, 10, 20);

  int v1 = r1.get().cast_<int>();
  int v2 = r2.get().cast_<int>();

  std::cout << "v1:" << v1 << std::endl;
  std::cout << "v2:" << v2 << std::endl;

  return 0;
}
