# Thread Pool Lifecycle

这份文档用流程图描述一次任务从提交到完成，以及线程池关闭时的主要路径。

## Task Flow

```mermaid
flowchart TD
  A[submit callable] --> B[deduce ReturnType]
  B --> C[wrap into packaged_task]
  C --> D[get future]
  D --> E{queue has space?}
  E -- yes --> F[enqueue task wrapper]
  E -- no --> G[wait up to 1 second]
  G --> H{space available?}
  H -- no --> I[throw submit timeout]
  H -- yes --> F
  F --> J[notify one worker]
  J --> K[worker pops task]
  K --> L[notify notFull]
  L --> M[execute packaged_task]
  M --> N[future becomes ready]
  N --> O[caller gets result or exception]
```

## Cached Worker Flow

```mermaid
flowchart TD
  A[worker enters wait loop] --> B{task queue empty?}
  B -- no --> C[pop and execute task]
  B -- yes --> D{cached extra worker?}
  D -- no --> E[wait until notified]
  D -- yes --> F[wait for idle timeout]
  F --> G{still empty and above initial size?}
  G -- no --> A
  G -- yes --> H[decrement counters]
  H --> I[mark WorkerState finished]
  I --> J[thread function returns]
  J --> K[later submit or scale path reaps worker]
  K --> L[join std::thread outside lock]
  L --> M[erase worker from vector]
```

## Shutdown Flow

```mermaid
flowchart TD
  A[shutdown called] --> W{called from worker task?}
  W -- yes --> X[throw runtime_error]
  W -- no --> B[lock taskQueMtx]
  B --> C{already shutting down?}
  C -- yes --> D[wait until shutdown finishes]
  C -- no --> E[set isPoolRunning false]
  E --> F[set isShuttingDown true]
  F --> G[notify workers and waiting submitters]
  G --> H[move worker list out]
  H --> I[join workers outside lock]
  I --> J[reset thread counters]
  J --> K[set isShuttingDown false]
  K --> L[notify waiters]
```

## Notes

- `submit()` 在等待队列空位时会释放锁，所以醒来后必须重新检查线程池是否仍在运行。
- `shutdown()` 不清空任务队列；已经入队的任务会继续执行。
- `shutdown()` 不允许从线程池任务内部调用。
- `shutdown()` 完成后，同一个线程池对象可以再次 `start()`。
- `join` 不能在持有 `taskQueMtx_` 的状态下执行。
- cached worker 退出时只标记自己结束，真正的 `std::thread` 回收由线程池后续完成。
