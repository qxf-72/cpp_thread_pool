<div align="center">

# cpp_thread_pool

一个基于 **C++17** 的轻量级线程池，用于学习和实践 C++ 并发编程。

支持 `std::future`、`std::packaged_task`、固定线程模式、Cached 动态扩容模式、任务异常传递、任务队列限流、1 秒提交超时拒绝、显式关闭和空闲线程自动回收。

![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.10%2B-brightgreen.svg)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

</div>

---

## 📖 项目简介

`cpp_thread_pool` 是一个小型 C++ 线程池项目，核心目标是展示线程池的基本组成：

- 工作线程如何等待和消费任务；
- 用户任务如何通过 `std::future<T>` 返回结果；
- 异常如何从工作线程传递回提交线程；
- 固定线程池和动态扩容线程池有什么区别；
- 队列满、线程池关闭、空闲线程回收等边界情况如何处理。

当前实现适合学习和小型实验，不建议未经充分测试直接用于生产环境。

## ✨ 功能特性

- 基于 C++17 标准库实现；
- 支持 `MODE_FIXED` 固定线程数模式；
- 支持 `MODE_CACHED` 动态扩容模式；
- 支持 cached 模式下空闲线程超时回收；
- 支持设置任务队列容量上限；
- 队列满时，`submit()` 最多阻塞 1 秒，超时后抛出异常表示提交失败；
- 支持提交 Lambda、普通函数、函数对象和带参数任务；
- 自动推导任务返回类型，返回 `std::future<T>`；
- 支持 `void` 返回值任务；
- 用户任务抛出的异常会在 `future.get()` 时重新抛出；
- 支持显式 `shutdown()`，析构时也会自动执行关闭流程；
- 禁止拷贝线程池对象，避免线程资源所有权混乱。

## 📁 项目结构

```text
cpp_thread_pool/
├── include/
│   └── threadpool.h      # 线程池接口和 submit() 模板实现
├── src/
│   └── threadpool.cpp    # 线程池生命周期、工作线程和回收逻辑
├── example/
│   └── example.cpp       # 使用示例
├── CMakeLists.txt
├── README.md
└── LICENSE
```

## 🧰 环境要求

| 工具 | 要求 |
| --- | --- |
| C++ 标准 | C++17 或更高 |
| CMake | 3.10 或更高 |
| 编译器 | GCC / Clang / MSVC |

推荐使用 GCC 9+、Clang 10+ 或 MSVC 2019+。

## 🚀 编译运行

生成构建目录：

```bash
cmake -S . -B build
```

编译：

```bash
cmake --build build
```

运行示例：

```bash
# Windows
./build/threadpool_demo.exe

# Linux / macOS
./build/threadpool_demo
```

## 💡 快速开始

### 固定线程模式

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

`submit()` 会自动推导返回类型。上例中 `result` 的类型是 `std::future<int>`。

### 提交带参数任务

```cpp
auto result = pool.submit([](int a, int b) {
  return a + b;
}, 10, 20);

int value = result.get();
```

### 提交普通函数

```cpp
int add(int a, int b) {
  return a + b;
}

auto result = pool.submit(add, 3, 4);
int value = result.get();
```

### 提交 void 任务

```cpp
auto result = pool.submit([] {
  // do something
});

result.get();  // 等待任务执行完成
```

### 任务异常处理

任务中抛出的异常会被 `std::packaged_task` 保存，并在调用 `future.get()` 时重新抛出。

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

## ⚡ Cached 模式

Cached 模式会根据任务压力动态增加线程数量。

当任务队列中积压的任务数量超过空闲线程数量，并且当前线程数还没有达到上限时，线程池会尝试创建新的工作线程。

当动态创建出来的线程空闲超过指定时间后，会自动退出；线程池随后会回收对应的 `std::thread` 对象，避免已结束线程句柄一直留在容器中。

```cpp
ThreadPool pool;
pool.setMode(ThreadPoolMode::MODE_CACHED);
pool.setThreadSizeThreshHold(8);
pool.setThreadMaxIdleTime(std::chrono::seconds(1));
pool.start(2);
```

配置含义：

| 配置 | 含义 |
| --- | --- |
| `pool.start(2)` | 初始创建 2 个工作线程 |
| `setThreadSizeThreshHold(8)` | 任务压力变大时最多扩容到 8 个线程 |
| `setThreadMaxIdleTime(std::chrono::seconds(1))` | 多余线程空闲超过 1 秒后自动退出 |

## ⏱️ 任务提交与拒绝策略

`submit()` 的行为如下：

1. 如果线程池还没有启动，抛出 `std::runtime_error("Thread pool is not running")`。
2. 如果队列已满，提交线程最多等待 1 秒。
3. 如果 1 秒内仍没有队列空位，抛出 `std::runtime_error("Task queue is full; submit timed out")`。
4. 如果等待期间线程池被关闭，抛出 `std::runtime_error("Thread pool has stopped")`。

示例：

```cpp
ThreadPool pool;
pool.setTaskQueMaxThreshHold(1);
pool.start(1);

try {
  auto result = pool.submit([] {
    return 100;
  });
} catch (const std::exception &e) {
  // 队列满、线程池未启动或线程池已关闭时会进入这里。
}
```

## 🛑 关闭语义

线程池支持显式关闭：

```cpp
pool.shutdown();
```

`shutdown()` 的语义是：

- 停止接收新任务；
- 已经进入队列的任务会继续执行；
- 等待工作线程退出；
- 回收线程资源；
- 可以重复调用。

如果用户没有手动调用 `shutdown()`，`ThreadPool` 析构时也会自动执行同样的关闭流程。

## 📚 API 说明

### 配置接口

所有配置接口都应在 `start()` 之前调用。

| API | 说明 |
| --- | --- |
| `setMode(ThreadPoolMode mode)` | 设置线程池模式，支持 `MODE_FIXED` 和 `MODE_CACHED` |
| `setTaskQueMaxThreshHold(std::size_t threshold)` | 设置任务队列最大容量 |
| `setThreadSizeThreshHold(std::size_t threshold)` | 设置 cached 模式下的最大线程数 |
| `setThreadMaxIdleTime(std::chrono::seconds idleTime)` | 设置 cached 模式下多余线程的最大空闲时间 |
| `start(std::size_t initThreadSize = 4)` | 启动线程池并创建初始工作线程 |

### 任务接口

| API | 返回值 | 说明 |
| --- | --- | --- |
| `submit(F&& func, Args&&... args)` | `std::future<T>` | 提交任意可调用对象，并自动推导返回类型 |

示例：

```cpp
auto f1 = pool.submit([] { return 100; });              // std::future<int>
auto f2 = pool.submit([] { return std::string("ok"); }); // std::future<std::string>
auto f3 = pool.submit([] {});                           // std::future<void>
```

### 状态查询接口

| API | 说明 |
| --- | --- |
| `currentThreadSize() const` | 获取当前线程池中的线程总数 |
| `idleThreadSize() const` | 获取当前空闲线程数量 |

这些接口主要用于学习、调试和观察 cached 模式的扩容与回收。

### 关闭接口

| API | 说明 |
| --- | --- |
| `shutdown()` | 停止接收新任务，等待已入队任务完成，并回收线程资源 |

## 🧩 核心设计

### 任务包装

用户提交的任意可调用对象都会被包装成 `std::packaged_task<ReturnType()>`：

```cpp
auto task = std::make_shared<std::packaged_task<ReturnType()>>(...);
std::future<ReturnType> result = task->get_future();
```

任务队列内部统一保存 `std::function<void()>`：

```cpp
std::queue<std::function<void()>> taskQue_;
```

工作线程执行队列任务时，本质上是在调用：

```cpp
(*task)();
```

返回值和异常都由 `std::packaged_task` 自动写入对应的 `std::future`。

### 工作线程流程

```text
等待任务队列非空
        ↓
取出一个任务
        ↓
唤醒可能等待队列空位的提交线程
        ↓
执行用户任务
        ↓
继续等待下一个任务
```

### Cached 线程回收

Cached 模式下，超过初始线程数的工作线程不会永久等待任务。

当它空闲超过 `threadMaxIdleTime_`，且当前线程数仍大于初始线程数时，它会：

1. 更新线程计数；
2. 标记自己的 `WorkerState::finished`；
3. 退出线程函数；
4. 在线程池后续提交或扩容时被 `reapFinishedThreadsLocked()` join 并从 `threads_` 中移除。

## ⚠️ 注意事项

1. `setMode()`、`setTaskQueMaxThreshHold()`、`setThreadSizeThreshHold()`、`setThreadMaxIdleTime()` 都需要在 `start()` 之前调用。
2. `submit()` 可能抛出异常，建议在队列容量较小或任务可能阻塞时进行捕获。
3. `std::future::get()` 只能调用一次，这是标准库 `future` 的语义。
4. 如果用户任务抛出异常，需要在调用 `future.get()` 的地方捕获。
5. `shutdown()` 会等待已入队任务执行完；如果任务内部永久阻塞，关闭流程也会等待。
6. 当前实现没有任务取消、任务优先级和工作窃取机制。

## 🗺️ 后续改进方向

- 增加单元测试和压力测试；
- 增加任务取消机制；
- 支持优先级任务队列；
- 支持更灵活的提交超时时间配置；
- 补充 GitHub Actions 自动构建；
- 增加性能基准测试。

## 📄 License

本项目基于 [MIT License](LICENSE) 开源。
