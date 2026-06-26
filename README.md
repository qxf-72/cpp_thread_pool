<div align="center">

# 🧵 cpp_thread_pool

一个基于 C++17 实现的轻量级线程池。

支持提交 Lambda、普通函数及其他可调用对象，支持参数绑定、任意类型返回值、异常传递、任务队列容量限制和线程安全退出。



![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/CMake-3.10%2B-brightgreen.svg)
![Platform](https://img.shields.io/badge/Platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

⭐ 如果这个项目对你有帮助，欢迎点一个 Star！

</div>

---

## 📖 项目简介

`cpp_thread_pool` 是一个使用 C++17 编写的轻量级线程池。

线程池启动后，会预先创建固定数量的工作线程。用户可以通过 `submit()` 提交 Lambda、普通函数、函数对象以及带参数的可调用对象。

提交的任务会进入线程安全的共享任务队列，空闲工作线程负责取出并执行任务。`submit()` 会返回一个 `Result` 对象，用户可以通过 `Result::get()` 阻塞等待任务完成，并获取任务返回值。

由于不同任务可能返回 `int`、`std::string` 或自定义类型，项目使用自定义 `Any` 类型对返回值进行类型擦除。

本项目主要用于学习和展示线程池的核心设计与运行流程，不建议未经充分测试直接用于生产环境。

## ✨ 功能特性

* 🧵 固定数量的工作线程
* 📦 线程安全的任务队列
* λ 支持提交 Lambda 表达式
* 🔧 支持普通函数和函数对象
* 📥 支持向任务传递任意数量的参数
* 🎁 支持任意类型的任务返回值
* ⏳ 支持阻塞等待任务执行结果
* 🛡️ 支持任务异常传递
* 🚦 支持任务队列容量限制
* 💤 使用条件变量避免线程忙等待
* 🔐 线程池析构时安全停止并回收线程
* 🚫 禁止线程池对象拷贝

## 🧰 环境要求

* C++17 或更高版本
* CMake 3.10 或更高版本
* 支持 C++ 标准线程库的编译器

推荐环境：

```text
GCC 9+
Clang 10+
MSVC 2019+
```

## 📁 项目结构

```text
cpp_thread_pool/
├── include/
│   └── threadpool.h
├── src/
│   └── threadpool.cpp
├── examples/
│   └── main.cpp
├── CMakeLists.txt
├── README.md
├── README.zh-CN.md
└── LICENSE
```

如果当前项目尚未拆分目录，也可以使用：

```text
cpp_thread_pool/
├── threadpool.h
├── threadpool.cpp
├── main.cpp
├── CMakeLists.txt
├── README.md
├── README.zh-CN.md
└── LICENSE
```

## 🚀 快速开始

### 1️⃣ 克隆仓库

```bash
git clone git@github.com:qxf-72/cpp_thread_pool.git
cd cpp_thread_pool
```

### 2️⃣ 编译项目

```bash
cmake -S . -B build
cmake --build build
```

### 3️⃣ 运行示例

```bash
./build/thread_pool_demo
```

## 💡 使用示例

### 提交无参数 Lambda

```cpp
#include <chrono>
#include <iostream>
#include <thread>

#include "threadpool.h"

int main() {
  ThreadPool pool;
  pool.start(2);

  auto result = pool.submit([] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 42;
  });

  int value = result.get().cast_<int>();

  std::cout << "result: " << value << '\n';

  return 0;
}
```

### 提交带参数 Lambda

```cpp
#include <iostream>

#include "threadpool.h"

int main() {
  ThreadPool pool;
  pool.start(2);

  auto result =
      pool.submit([](int a, int b) {
        return a + b;
      }, 10, 20);

  int value = result.get().cast_<int>();

  std::cout << "result: " << value << '\n';

  return 0;
}
```

### 同时提交多个任务

```cpp
#include <chrono>
#include <iostream>
#include <thread>

#include "threadpool.h"

int main() {
  ThreadPool pool;
  pool.start(2);

  auto r1 = pool.submit([] {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 42;
  });

  auto r2 = pool.submit(
      [](int a, int b) {
        return a + b;
      },
      10, 20);

  int v1 = r1.get().cast_<int>();
  int v2 = r2.get().cast_<int>();

  std::cout << "v1: " << v1 << '\n';
  std::cout << "v2: " << v2 << '\n';

  return 0;
}
```

可能的输出：

```text
v1: 42
v2: 30
```

### 提交普通函数

```cpp
#include <iostream>

#include "threadpool.h"

int multiply(int a, int b) {
  return a * b;
}

int main() {
  ThreadPool pool;
  pool.start(2);

  auto result = pool.submit(multiply, 6, 7);

  int value = result.get().cast_<int>();

  std::cout << "result: " << value << '\n';

  return 0;
}
```

## 🔄 工作流程

```text
创建 ThreadPool
        │
        ▼
调用 start() 创建工作线程
        │
        ▼
调用 submit(function, args...)
        │
        ▼
绑定可调用对象及其参数
        │
        ▼
创建对应的 ResultState
        │
        ▼
任务进入共享任务队列
        │
        ▼
条件变量唤醒工作线程
        │
        ▼
工作线程执行任务
        │
        ▼
返回值写入 ResultState
        │
        ▼
Result::get() 获取任务结果
```

## 🧩 核心组件

### 🧵 ThreadPool

线程池主体，负责：

* 创建和管理工作线程
* 接收用户提交的可调用对象
* 将任务和参数封装成无参数任务
* 维护线程安全的任务队列
* 阻塞和唤醒工作线程
* 在线程池析构时安全回收线程

用户通过以下接口提交任务：

```cpp
template <typename Func, typename... Args>
Result submit(Func&& func, Args&&... args);
```

其中：

* `Func` 表示可调用对象类型
* `Args...` 表示传递给任务的参数类型
* `Result` 表示任务执行结果

### 📦 任务队列

线程池内部维护一个共享任务队列。

`submit()` 会将用户传入的函数和参数绑定为一个可供工作线程直接执行的任务，再将任务放入队列。

当任务队列为空时，工作线程阻塞在 `notEmpty_` 条件变量上，避免持续循环占用 CPU。

当任务队列达到容量上限时，提交任务的线程会阻塞在 `notFull_` 条件变量上，直到队列重新出现空位。

### 🎫 Result

`submit()` 返回一个 `Result` 对象：

```cpp
auto result = pool.submit([] {
  return 100;
});
```

用户可以通过：

```cpp
Any value = result.get();
```

等待任务执行完成。

如果任务尚未完成，`get()` 会阻塞当前调用线程；任务执行完成后，`get()` 返回保存结果的 `Any` 对象。

### 📬 ResultState

`ResultState` 是用户线程和工作线程之间共享的结果状态。

它主要保存：

* 任务返回值
* 任务是否已经完成
* 返回值是否已经被消费
* 任务执行期间产生的异常
* 用于等待和唤醒的条件变量

工作线程完成任务后调用：

```cpp
resultState->setValue(...);
```

用户线程通过：

```cpp
result.get();
```

等待并获取结果。

### 🎁 Any

`Any` 是一个自定义类型擦除容器，用于统一保存不同类型的任务返回值。

保存整数：

```cpp
Any value = 100;
int number = value.cast_<int>();
```

保存字符串：

```cpp
Any value = std::string("hello");
std::string message = value.cast_<std::string>();
```

如果取出类型与实际类型不匹配，会抛出异常：

```cpp
Any value = 100;

// 抛出 std::runtime_error
std::string message = value.cast_<std::string>();
```

### 🔧 可调用对象封装

`submit()` 使用模板和完美转发接收不同类型的任务：

```cpp
pool.submit(func, arg1, arg2, ...);
```

用户无需继承任务基类，也无需手动创建任务对象。

例如：

```cpp
pool.submit([] {
  return 42;
});
```

以及：

```cpp
pool.submit([](int a, int b) {
  return a + b;
}, 10, 20);
```

线程池会在内部完成函数与参数的绑定，并在工作线程中执行。

## 🔐 线程安全退出

线程池析构时会执行以下流程：

```text
将 isPoolRunning_ 设置为 false
        │
        ▼
唤醒所有正在等待的线程
        │
        ▼
工作线程继续处理队列中的剩余任务
        │
        ▼
任务队列为空后退出
        │
        ▼
主线程调用 join() 等待工作线程结束
```

典型的工作线程退出条件为：

```cpp
if (!isPoolRunning_ && taskQue_.empty()) {
  return;
}
```

这意味着线程池停止后，已经成功提交到任务队列中的任务仍会被执行完毕。

## 🛡️ 异常处理

用户提交的任务可能抛出异常：

```cpp
auto result = pool.submit([]() -> int {
  throw std::runtime_error("task failed");
});
```

工作线程会捕获异常并将其保存到对应的 `ResultState` 中，避免工作线程因为用户任务异常而直接终止。

当用户调用：

```cpp
result.get();
```

异常会在调用 `get()` 的线程中重新抛出：

```cpp
try {
  int value = result.get().cast_<int>();
} catch (const std::exception& e) {
  std::cerr << e.what() << '\n';
}
```

## ⚠️ 注意事项

### Result 只能消费一次

当前 `Any` 是只移动类型，`Result::get()` 会将保存的结果移动给调用者。

因此，同一个 `Result` 只能成功调用一次 `get()`：

```cpp
Any first = result.get();
Any second = result.get();  // 抛出异常
```

这种行为与 `std::future::get()` 类似。

### 必须使用正确类型提取结果

任务返回什么类型，就需要使用相同类型调用 `cast_()`：

```cpp
auto result = pool.submit([] {
  return 42;
});

int value = result.get().cast_<int>();
```

下面的代码会抛出类型不匹配异常：

```cpp
std::string value = result.get().cast_<std::string>();
```

### 当前只支持固定线程模式

当前版本只实现固定数量工作线程：

```cpp
ThreadPoolMode::MODE_FIXED
```

`MODE_CACHED` 动态线程模式尚未实现。

### 线程池对象只能启动一次

建议每个 `ThreadPool` 对象只调用一次：

```cpp
pool.start(4);
```

当前版本不支持停止后重新启动。

## 🚧 当前限制

当前版本暂不支持：

* cached 动态线程模式
* 空闲线程超时回收
* 任务取消
* 任务优先级
* 工作窃取
* 无锁任务队列
* Result 多次读取
* 线程池停止后重新启动
* 多种任务拒绝策略

## 🗺️ 后续计划

* [x] 支持 Lambda 表达式提交
* [x] 支持普通函数和函数对象
* [x] 支持任务参数传递
* [x] 支持任意类型返回值
* [x] 支持任务异常传递
* [ ] 实现 cached 动态线程模式
* [ ] 支持空闲线程超时回收
* [ ] 增加任务提交超时机制
* [ ] 增加多种任务拒绝策略
* [ ] 支持任务优先级
* [ ] 增加线程池运行状态统计
* [ ] 增加单元测试
* [ ] 增加性能测试
* [ ] 增加 GitHub Actions 自动构建
* [ ] 完善 API 文档

## 📚 学习重点

通过这个项目可以学习：

* `std::thread` 的创建与回收
* `std::mutex` 和 RAII 锁管理
* `std::condition_variable`
* 生产者—消费者模型
* 线程安全任务队列
* 可变参数模板
* 完美转发
* 可调用对象和参数绑定
* 智能指针和对象生命周期
* 类型擦除
* 异常在线程之间的传递
* 线程池的安全启动和退出

## 📊 开发状态

本项目目前处于学习和持续完善阶段。

当前版本适合：

* 学习 C++ 并发编程
* 理解线程池核心流程
* 学习可变参数模板与完美转发
* 练习条件变量和生产者—消费者模型
* 作为 Linux C++ 后端学习项目

> [!WARNING]
> 本项目主要用于学习和技术交流，不建议未经充分测试直接用于生产环境。

## 🤝 贡献

欢迎提交 Issue 或 Pull Request。

提交代码前建议：

1. 确保代码能够正常编译。
2. 保持现有代码风格。
3. 为新增功能补充必要测试。
4. 在 Pull Request 中说明修改目的和实现方式。

## 📄 许可证

本项目基于 [MIT License](LICENSE) 开源。

---

<div align="center">

如果这个项目对你有帮助，欢迎 ⭐ Star、🍴 Fork 或提交 Issue。

</div>
