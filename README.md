# UART Control

一个轻量级、芯片无关的 UART 收发控制模块。模块本身不直接依赖具体 MCU 或 HAL，而是通过 `struct uart_control_hal_ops` 注入底层硬件操作，适合放进不同嵌入式工程中复用。

## 特性

- 硬件无关：发送、启动接收、停止接收、读取字节都由外部 HAL 适配。
- 支持多实例：回调函数会带回 `struct uart_control *uart`，方便区分不同 UART。
- 基于空闲超时判断帧结束：逐字节接收，定时器超时后认为一帧完成。
- 接收缓冲区溢出可检测：帧长超过 `rx_size` 时会设置溢出标志。
- 发送忙状态可检测：重复发送会返回 `UART_CONTROL_BUSY`。
- 公共 API 带基本参数检查，降低误用风险。

## 目录结构

```text
.
├── platform.h
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

static uint8_t uart1_rx_buf[128];
static uint8_t uart1_tx_buf[128];
```

发送前，应用层需要把待发送数据写入 `uart1_tx_buf`，再调用 `uart_control_send()`。

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
                          uart1_tx_buf,
                          sizeof(uart1_tx_buf),
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

```c
memcpy(uart1_tx_buf, data, len);

switch (uart_control_send(&uart1, len))
{
case UART_CONTROL_OK:
    break;

case UART_CONTROL_BUSY:
    /* 上一次发送还没有完成 */
    break;

default:
    /* 参数错误，例如 len 为 0 或超过 tx_size */
    break;
}
```

## 主要 API

| 函数 | 说明 |
| --- | --- |
| `uart_control_init()` | 初始化 UART 控制实例，绑定 HAL 操作、缓冲区、超时时间和回调 |
| `uart_control_enable_rx()` | 启动一帧接收，并清空接收状态 |
| `uart_control_send()` | 启动一帧发送 |
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

- `uart_control_send()` 只负责发送 `tx_buf` 中已有的数据，不会主动拷贝外部数据。
- `uart_control_rx_read()` 读完当前帧后会返回 `false`，读指针会在下一次 `uart_control_enable_rx()` 时复位。
- 如果 `uart_control_rx_overflow()` 返回 `true`，说明当前帧长度超过 `rx_size`，缓冲区内只保留了前 `rx_size` 字节。
- `uart_control_timer_isr()` 的调用周期决定了 `timeout_tick` 的实际时间单位。
- ISR 入口通常应在 `uart_control_init()` 成功后再调用。

