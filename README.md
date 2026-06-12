# cpp_thread_pool

[中文文档](README.zh-CN.md)

A minimal thread pool implemented in C++11.

This project is a simple C++ thread pool demo. It supports submitting `std::function<void()>` tasks to a fixed number of worker threads. The implementation focuses on the core ideas of thread pool design, including task queue, mutex, condition variable, worker threads, and graceful shutdown.

## Features

* Fixed number of worker threads
* Task submission with `std::function<void()>`
* Thread-safe task queue
* Worker thread blocking with `std::condition_variable`
* Graceful shutdown in destructor
* Simple and easy to understand

## Requirements

* C++11 or later
* Linux / macOS / Windows with C++ compiler
* pthread support on Linux



## Example

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

## Core Idea

The main idea of this project is simple:

1. The main thread submits tasks into a shared task queue.
2. Worker threads wait for tasks using a condition variable.
3. When a task arrives, one worker thread wakes up and executes it.
4. When the thread pool is destroyed, all worker threads exit safely.

## Project Structure

```text
cpp_thread_pool/
├── include/
│   └── ThreadPool.h
├── test/
│   └── test.cpp
├── main.cpp
├── README.md
└── LICENSE
```

## Notes

This is a minimal version of a thread pool. It does not support task return values or `std::future`.

Possible future improvements:

* Support `std::future`
* Support tasks with return values
* Add task count statistics
* Add stop modes
* Add unit tests

## License

This project is licensed under the MIT License.
