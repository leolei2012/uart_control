# UART Control

一个轻量级、芯片无关的 UART 收发控制模块。模块本身不直接依赖具体 MCU 或 HAL，而是通过 `struct uart_control_hal_ops` 注入底层硬件操作，适合放进不同嵌入式工程中复用。

## 特性

- 硬件无关：发送、启动接收、停止接收、读取字节都由外部 HAL 适配。
- 支持多实例：回调函数会带回 `struct uart_control *uart`，方便区分不同 UART。
- 基于空闲超时判断帧结束：逐字节接收，定时器超时后认为一帧完成。
- 接收缓冲区溢出可检测：帧长超过 `rx_size` 时会设置溢出标志。
- 发送忙状态可检测：重复发送会返回 `UART_CONTROL_BUSY`。
- 公共 API 带基本参数检查，降低误用风险。

## 设计说明：为什么只提供 RX 缓冲，不提供 TX 缓冲

- **接收（RX）必须由模块持有缓冲**：接收是异步的，数据随时从线上进入，DMA/中断必须写入一个常驻、始终就绪的缓冲，所以该缓冲由调用方在 `uart_control_init()` 时提供（`rx_buf`/`rx_size`）。
- **发送（TX）由应用传入指针即可**：发送是应用主动发起的，应用已经把帧组好在自己的 buffer 里，发送时把指针传给 `uart_control_send(buf, len)`，DMA 直接读它，无需再拷贝到模块内部缓冲。
- **代价（调用方约束）**：由于零拷贝，调用方必须保证 `buf` 在发送完成（`on_tx_done_callback` 触发、或 `tx_busy` 清零）之前不被改写/释放。

## 目录结构

```text
.
└── uart_control
    ├── include
    │   └── uart_control.h
    └── src
        └── uart_control.c
```

## 快速接入

### 1. 包含头文件

```c
#include "uart_control.h"
```

编译时需要加入头文件路径：

```text
.
uart_control/include
```

并把 `uart_control/src/uart_control.c` 加入工程编译。

### 2. 准备缓冲区和 UART 实例

```c
static struct uart_control uart1;

static uint8_t uart1_rx_buf[128];   /* 只需接收缓冲 */
```

模块只持有接收缓冲（接收是异步的，必须常驻就绪）。发送不设独立缓冲，应用层发送时直接把自己组好帧的 buffer 指针传给 `uart_control_send()`（零拷贝，见「设计说明」）。

### 3. 适配底层硬件操作

根据实际平台实现下面 4 个函数：

```c
static void uart1_send(const uint8_t *buf, uint16_t len)
{
    /* 调用芯片 HAL 启动 UART 发送 */
}

static void uart1_rx_start(void)
{
    /* 启动 UART 单字节接收中断或 DMA 接收 */
}

static void uart1_rx_stop(void)
{
    /* 停止 UART 接收 */
}

static uint8_t uart1_read_byte(void)
{
    /* 从 UART 接收寄存器或 HAL 缓冲区读取 1 字节 */
    return 0u;
}

static const struct uart_control_hal_ops uart1_ops =
{
    .send = uart1_send,
    .rx_start = uart1_rx_start,
    .rx_stop = uart1_rx_stop,
    .read_byte = uart1_read_byte,
};
```

### 4. 初始化模块

```c
static void uart1_rx_done(struct uart_control *uart, uint16_t len)
{
    uint8_t byte;

    if (uart_control_rx_overflow(uart))
    {
        /* 当前帧超过 RX 缓冲区，数据已被截断 */
    }

    while (uart_control_rx_read(uart, &byte))
    {
        /* 处理接收到的 byte */
    }

    /* 如果需要连续接收，处理完成后重新启动下一帧接收 */
    uart_control_enable_rx(uart);
}

static void uart1_tx_done(struct uart_control *uart)
{
    (void)uart;
    /* 发送完成处理 */
}

void uart1_control_init(void)
{
    if (uart_control_init(&uart1,
                          &uart1_ops,
                          uart1_rx_buf,
                          sizeof(uart1_rx_buf),
                          5u,
                          uart1_rx_done,
                          uart1_tx_done) == UART_CONTROL_OK)
    {
        uart_control_enable_rx(&uart1);
    }
}
```

`timeout_tick` 的单位取决于你调用 `uart_control_timer_isr()` 的周期。例如定时器每 1 ms 调用一次，`timeout_tick = 5` 表示接收空闲 5 ms 后认为一帧结束。

## 中断/定时器接入

在平台的中断入口中调用模块提供的 ISR 入口：

```c
void UART_RX_IRQHandler(void)
{
    uart_control_rx_isr(&uart1);
}

void UART_TX_DONE_IRQHandler(void)
{
    uart_control_tx_done_isr(&uart1);
}

void TIMER_IRQHandler(void)
{
    uart_control_timer_isr(&uart1);
}
```

## 发送数据

应用层组好帧后，直接把 buffer 指针和长度传给 `uart_control_send()`（零拷贝，DMA 直接读该 buffer）：

```c
uint8_t tx_frame[128];   /* 应用层自己的发送帧缓冲 */

/* ... 填充 tx_frame ... */

switch (uart_control_send(&uart1, tx_frame, len))
{
case UART_CONTROL_OK:
    break;

case UART_CONTROL_BUSY:
    /* 上一次发送还没有完成 */
    break;

default:
    /* 参数错误，例如 buf 为 NULL 或 len 为 0 */
    break;
}

/* 注意：DMA 完成前不要改写 tx_frame（见「设计说明」） */
```

## 主要 API

| 函数 | 说明 |
| --- | --- |
| `uart_control_init()` | 初始化 UART 控制实例，绑定 HAL 操作、缓冲区、超时时间和回调 |
| `uart_control_enable_rx()` | 启动一帧接收，并清空接收状态 |
| `uart_control_send()` | 零拷贝发送一帧（DMA 直接读传入的 buf） |
| `uart_control_rx_len()` | 获取当前帧总长度 |
| `uart_control_rx_remaining()` | 获取当前帧剩余未读字节数 |
| `uart_control_rx_read()` | 读取当前帧中的下一个字节 |
| `uart_control_rx_overflow()` | 查询当前帧是否发生接收缓冲区溢出 |
| `uart_control_bind_rx_done_callback()` | 运行时更换接收完成回调 |
| `uart_control_bind_tx_done_callback()` | 运行时更换发送完成回调 |
| `uart_control_rx_isr()` | 接收中断入口 |
| `uart_control_tx_done_isr()` | 发送完成中断入口 |
| `uart_control_timer_isr()` | 空闲超时定时器入口 |

## 返回值

| 返回值 | 含义 |
| --- | --- |
| `UART_CONTROL_OK` | 操作成功 |
| `UART_CONTROL_ERROR` | 参数非法或状态不完整 |
| `UART_CONTROL_TIMEOUT` | 预留的超时状态 |
| `UART_CONTROL_BUSY` | UART 正在发送，暂时不能启动新的发送 |

## 接收流程

1. 调用 `uart_control_enable_rx()` 启动接收。
2. UART 每收到 1 字节，在接收中断里调用 `uart_control_rx_isr()`。
3. `uart_control_rx_isr()` 保存字节，并把空闲超时计数清零。
4. 定时器周期性调用 `uart_control_timer_isr()`。
5. 当连续空闲时间达到 `timeout_tick` 后，模块停止接收并调用 `on_rx_done_callback(uart, len)`。
6. 应用层在回调中读取数据，处理完成后根据需要再次调用 `uart_control_enable_rx()`。

## 注意事项

- `uart_control_send()` 零拷贝发送：DMA 直接读传入的 `buf`，不拷贝；调用方必须保证 `buf` 在发送完成前保持有效。
- `uart_control_rx_read()` 读完当前帧后会返回 `false`，读指针会在下一次 `uart_control_enable_rx()` 时复位。
- 如果 `uart_control_rx_overflow()` 返回 `true`，说明当前帧长度超过 `rx_size`，缓冲区内只保留了前 `rx_size` 字节。
- `uart_control_timer_isr()` 的调用周期决定了 `timeout_tick` 的实际时间单位。
- ISR 入口通常应在 `uart_control_init()` 成功后再调用。

