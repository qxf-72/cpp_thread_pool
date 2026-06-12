# cpp_thread_pool

一个基于 C++11 实现的极简线程池。

本项目是一个简单的 C++ 线程池示例，支持向固定数量的工作线程提交 `std::function<void()>` 类型的任务。项目重点展示线程池的核心设计思想，包括任务队列、互斥锁、条件变量、工作线程以及析构时的优雅退出。

## 功能特性

* 固定数量的工作线程
* 支持提交 `std::function<void()>` 类型任务
* 使用互斥锁保护任务队列
* 使用条件变量实现线程阻塞与唤醒
* 析构时安全停止所有工作线程
* 代码结构简单，适合学习线程池基本原理

## 环境要求

* C++11 或更高版本
* Linux / macOS / Windows
* Linux 下编译需要 pthread 支持


## 使用示例

```cpp
#include <iostream>
#include "ThreadPool.h"

int main() {
    ThreadPool pool(4);

    for (int i = 0; i < 10; ++i) {
        pool.submit([i]() {
            std::cout << "task " << i
                      << " is running in thread "
                      << std::this_thread::get_id()
                      << std::endl;
        });
    }

    return 0;
}
```

## 核心思想

线程池的基本思想是：

1. 主线程将任务提交到共享任务队列中。
2. 工作线程通过条件变量等待任务。
3. 当任务到来时，某个工作线程被唤醒，并从任务队列中取出任务执行。
4. 当线程池对象析构时，通知所有工作线程退出，并等待它们结束。

## 项目结构

```text
cpp_thread_pool/
├── include/
│   └── threadpool.h
├── test/
│   └── test.cpp
├── README.md
├── README.zh-CN.md
└── LICENSE
```

## 主要成员说明

```cpp
std::vector<std::thread> workers_;
std::queue<std::function<void()>> tasks_;
std::mutex mtx_;
std::condition_variable cv_;
bool stop_;
```

其中：

* `workers_`：保存所有工作线程
* `tasks_`：保存等待执行的任务
* `mtx_`：保护任务队列，避免多线程同时访问导致数据竞争
* `cv_`：用于在线程没有任务时阻塞，有任务时唤醒
* `stop_`：表示线程池是否准备停止

## 当前限制

这是一个极简版本的线程池，当前不支持：

* 获取任务返回值
* `std::future`
* 任务异常统一处理
* 动态调整线程数量
* 任务优先级
* 任务队列容量限制

## 后续改进方向

后续可以继续扩展：

* 支持 `std::future`
* 支持带返回值的任务
* 支持任意可调用对象提交
* 增加任务数量统计
* 增加线程池运行状态查询
* 增加单元测试

## 许可证

本项目基于 MIT License 开源。
