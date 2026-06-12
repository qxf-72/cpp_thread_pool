

#include <iostream>

#include "threadpool.h"

using namespace std;

int main() {
  ThreadPool pool(4);

  for (int i = 0; i < 10; ++i) {
    pool.submit([i]() {
      std::cout << "task " << i << " is running in thread " << std::this_thread::get_id()
                << std::endl;
    });
  }

  return 0;
}
