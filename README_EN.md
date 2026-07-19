<div align="center">

# cpp_thread_pool

[English](README_EN.md) | [简体中文](README.md)

A lightweight **C++17** thread pool for learning and practicing C++ concurrent programming.

It supports `std::future`, `std::packaged_task`, fixed-size workers, cached dynamic scaling, task exception propagation, bounded task queues, configurable submit timeout rejection, explicit shutdown, and automatic reclamation of idle cached workers.

![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.10%2B-brightgreen.svg)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)
[![CI](https://github.com/qxf-72/cpp_thread_pool/actions/workflows/ci.yml/badge.svg)](https://github.com/qxf-72/cpp_thread_pool/actions/workflows/ci.yml)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

</div>

---

## 📖 Project Overview

`cpp_thread_pool` is a small C++ thread pool project. Its main goal is to demonstrate the essential parts of a thread pool:

- how worker threads wait for and consume tasks;
- how user tasks return results through `std::future<T>`;
- how exceptions are propagated from worker threads back to the submitting thread;
- how a fixed-size pool differs from a dynamically scaling cached pool;
- how edge cases such as a full queue, pool shutdown, and idle worker reclamation are handled.

The current implementation is suitable for learning and small experiments. It is not recommended for production use without sufficient testing.

## ✨ Features

- Implemented with the C++17 standard library;
- supports `MODE_FIXED` fixed-size worker mode;
- supports `MODE_CACHED` dynamic scaling mode;
- supports idle worker timeout and reclamation in cached mode;
- supports configuring the maximum task queue capacity;
- when the queue is full, `submit()` blocks for the configured timeout and throws an exception if the task cannot be submitted;
- supports submitting lambdas, free functions, function objects, and callables with arguments;
- automatically deduces task return types and returns `std::future<T>`;
- supports tasks with `void` return type;
- exceptions thrown by user tasks are rethrown when `future.get()` is called;
- supports explicit `shutdown()`, and also shuts down automatically in the destructor;
- disables copying of the thread pool object to avoid confusing thread ownership.

## 📁 Project Structure

```text
cpp_thread_pool/
├── include/
│   └── threadpool.h      # Thread pool interface and submit() template implementation
├── src/
│   └── threadpool.cpp    # Lifecycle, worker thread, and reclamation logic
├── examples/
│   └── example.cpp       # Usage example
├── tests/
│   └── threadpool_tests.cpp # Lightweight behavior tests
├── docs/
│   ├── design.md         # Design notes and trade-offs
│   └── threadpool_lifecycle.md # Submit/worker/shutdown flow
├── CMakeLists.txt
├── README.md          # English documentation
├── README_CN.md       # Simplified Chinese documentation
└── LICENSE
```

## 🧰 Requirements

| Tool | Requirement |
| --- | --- |
| C++ standard | C++17 or later |
| CMake | 3.10 or later |
| Compiler | GCC / Clang / MSVC |

GCC 9+, Clang 10+, or MSVC 2019+ is recommended.

## 🚀 Build and Run

Generate the build directory:

```bash
cmake -S . -B build
```

Build the project:

```bash
cmake --build build
```

Run the example:

```bash
# Windows
./build/threadpool_demo.exe

# Linux / macOS
./build/threadpool_demo
```

Run tests:

```bash
ctest --test-dir build --output-on-failure
```

## 💡 Quick Start

### Fixed Worker Mode

```cpp
#include "threadpool.h"

#include <iostream>

int main() {
  ThreadPool pool;
  pool.start(4);

  auto result = pool.submit([] {
    return 42;
  });

  std::cout << result.get() << std::endl;
  pool.shutdown();
}
```

`submit()` automatically deduces the return type. In the example above, `result` is a `std::future<int>`.

### Submit a Task with Arguments

```cpp
auto result = pool.submit([](int a, int b) {
  return a + b;
}, 10, 20);

int value = result.get();
```

### Submit a Free Function

```cpp
int add(int a, int b) {
  return a + b;
}

auto result = pool.submit(add, 3, 4);
int value = result.get();
```

### Submit a Void Task

```cpp
auto result = pool.submit([] {
  // do something
});

result.get();  // Wait until the task is finished.
```

### Task Exception Handling

Exceptions thrown inside a task are stored by `std::packaged_task` and rethrown when `future.get()` is called.

```cpp
auto result = pool.submit([]() -> int {
  throw std::runtime_error("task failed");
});

try {
  result.get();
} catch (const std::exception &e) {
  std::cerr << e.what() << std::endl;
}
```

## ⚡ Cached Mode

Cached mode dynamically increases the number of worker threads according to task pressure.

When the number of queued tasks is greater than the number of idle workers, and the current worker count has not reached the configured upper limit, the thread pool attempts to create new worker threads.

When a dynamically created worker remains idle for longer than the configured timeout, it exits automatically. The pool later reclaims the corresponding `std::thread` object, preventing finished thread handles from staying in the worker container forever.

```cpp
ThreadPool pool;
pool.setMode(ThreadPoolMode::MODE_CACHED);
pool.setThreadSizeThreshold(8);
pool.setThreadMaxIdleTime(std::chrono::seconds(1));
pool.start(2);
```

Configuration meaning:

| Configuration | Meaning |
| --- | --- |
| `pool.start(2)` | Initially create 2 worker threads |
| `setThreadSizeThreshold(8)` | Scale up to at most 8 workers when task pressure grows |
| `setThreadMaxIdleTime(std::chrono::seconds(1))` | Extra cached workers exit after being idle for more than 1 second |

## ⏱️ Task Submission and Rejection Policy

`submit()` behaves as follows:

1. If the pool has not been started, it throws `std::runtime_error("Thread pool is not running")`.
2. If the queue is full, the submitting thread waits for the configured timeout, which defaults to 1 second.
3. If no queue slot becomes available before the timeout expires, it throws `std::runtime_error("Task queue is full; submit timed out")`.
4. If the pool is shut down while the submitting thread is waiting, it throws `std::runtime_error("Thread pool has stopped")`.

Example:

```cpp
ThreadPool pool;
pool.setTaskQueMaxThreshold(1);
pool.setSubmitTimeout(std::chrono::milliseconds(500));
pool.start(1);

try {
  auto result = pool.submit([] {
    return 100;
  });
} catch (const std::exception &e) {
  // This block is reached when the queue is full,
  // the pool has not been started, or the pool has been shut down.
}
```

## 🛑 Shutdown Semantics

The thread pool supports explicit shutdown:

```cpp
pool.shutdown();
```

`shutdown()` means:

- stop accepting new tasks;
- continue executing tasks that have already entered the queue;
- wait for worker threads to exit;
- reclaim thread resources;
- allow repeated calls;
- allow the same `ThreadPool` object to be started again after shutdown completes.

If the user does not call `shutdown()` manually, the `ThreadPool` destructor performs the same shutdown process automatically.

`shutdown()` must be called from outside the worker tasks. Calling it from a task running inside the pool is rejected because it would make worker lifetime and object ownership ambiguous.

The `ThreadPool` object itself should also be owned and destroyed outside its worker tasks.

## 📚 API Reference

### Configuration APIs

All configuration APIs should be called before `start()`.

| API | Description |
| --- | --- |
| `setMode(ThreadPoolMode mode)` | Set the pool mode, either `MODE_FIXED` or `MODE_CACHED` |
| `setTaskQueMaxThreshold(std::size_t threshold)` | Set the maximum task queue capacity |
| `setSubmitTimeout(std::chrono::milliseconds timeout)` | Set how long `submit()` waits for a free queue slot when the queue is full |
| `setThreadSizeThreshold(std::size_t threshold)` | Set the maximum worker count in cached mode |
| `setThreadMaxIdleTime(std::chrono::seconds idleTime)` | Set the maximum idle time for extra cached workers |
| `start(std::size_t initThreadSize = 4)` | Start the pool and create initial worker threads |

### Task API

| API | Return Value | Description |
| --- | --- | --- |
| `submit(F&& func, Args&&... args)` | `std::future<T>` | Submit any callable object and automatically deduce its return type |

Examples:

```cpp
auto f1 = pool.submit([] { return 100; });               // std::future<int>
auto f2 = pool.submit([] { return std::string("ok"); }); // std::future<std::string>
auto f3 = pool.submit([] {});                            // std::future<void>
```

### State Query APIs

| API | Description |
| --- | --- |
| `currentThreadSize() const` | Get the current total number of workers in the pool |
| `idleThreadSize() const` | Get the current number of idle workers |

These APIs are mainly useful for learning, debugging, and observing scaling and reclamation behavior in cached mode.

### Shutdown API

| API | Description |
| --- | --- |
| `shutdown()` | Stop accepting new tasks, wait for queued tasks to finish, and reclaim thread resources |

## 🧩 Implementation Notes

Some details are intentionally kept out of the source comments and documented separately:

- [Design Notes](docs/design.md) explains the main trade-offs, including why this version uses `std::future` / `std::packaged_task`, why cached workers must be joined after exit, and why `shutdown()` does not simply clear the task queue.
- [Thread Pool Lifecycle](docs/threadpool_lifecycle.md) shows the `submit -> enqueue -> worker -> packaged_task -> future ready -> shutdown -> join` flow with Mermaid diagrams.

## 📝 Learning Notes

- An earlier experimental design considered custom `Any` / `Result`-style wrappers. This version uses `std::future` and `std::packaged_task` instead, so return values and exceptions follow standard library semantics.
- Cached workers cannot only decrement a counter when they exit. Their `std::thread` objects still have to be joined and removed from the worker list.
- `shutdown()` does not clear queued tasks directly. Already submitted tasks are allowed to finish; otherwise callers holding `future` objects would need a separate cancellation policy.
- `shutdown()` is an external lifecycle operation. A task running inside the pool is not allowed to call it.
- After `shutdown()` finishes, the same `ThreadPool` object can be started again.
- The submit timeout defaults to 1 second and can be changed with `setSubmitTimeout()`. It only controls how long `submit()` waits for a free queue slot when the queue is full.
- The current implementation does not support task cancellation, priority queues, or work stealing.

## ⚠️ Notes

1. `setMode()`, `setTaskQueMaxThreshold()`, `setSubmitTimeout()`, `setThreadSizeThreshold()`, and `setThreadMaxIdleTime()` must be called before `start()`.
2. `submit()` may throw exceptions, so callers should catch them when the queue capacity is small or tasks may block.
3. `std::future::get()` can be called only once. This is the standard library semantics of `future`.
4. If a user task throws an exception, it should be caught at the place where `future.get()` is called.
5. `shutdown()` waits for already queued tasks to finish. If a task blocks forever, shutdown will also wait.
6. Do not call `shutdown()` from inside a submitted task, and do not destroy the pool object from one of its worker tasks.
7. `submit()` stores the callable and arguments by copy or move. Use `std::ref` or `std::cref` if a task needs reference semantics.
8. The current implementation does not provide task cancellation, task priorities, or work stealing.

## 🗺️ Future Improvements

- Expand unit tests and add stress tests;
- add task cancellation;
- support priority task queues;
- support per-submit timeout duration;
- add sanitizer builds for memory and thread checks;
- add performance benchmarks.

## 📄 License

This project is open source under the [MIT License](LICENSE).
