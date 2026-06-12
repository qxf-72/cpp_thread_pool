#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool {
 public:
  explicit ThreadPool(size_t threadCount) : stop_(false) {
    for (size_t i = 0; i < threadCount; ++i) {
      workers_.emplace_back([this]() { this->workerLoop(); });
    }
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      stop_ = true;
    }

    cv_.notify_all();

    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  void submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mtx_);

      if (stop_) {
        throw std::runtime_error("ThreadPool has stopped");
      }

      tasks_.push(std::move(task));
    }

    cv_.notify_one();
  }

 private:
  void workerLoop() {
    while (true) {
      std::function<void()> task;

      {
        std::unique_lock<std::mutex> lock(mtx_);

        cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });

        if (stop_ && tasks_.empty()) {
          return;
        }

        task = std::move(tasks_.front());
        tasks_.pop();
      }

      task();
    }
  }

 private:
  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;

  std::mutex mtx_;
  std::condition_variable cv_;

  bool stop_;
};
