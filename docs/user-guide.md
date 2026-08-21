# uart_control 使用指南

`uart_control` 是一个轻量级、芯片无关的 UART 收发控制模块。本文面向把它接入自己工程的开发者，讲清楚**怎么用、为什么这么设计、有哪些坑**。

---

## 1. 一句话概括

- **接收（RX）**：模块持有接收缓冲，逐字节收，靠空闲超时判断一帧结束，回调通知你。
- **发送（TX）**：零拷贝。你把组好帧的 buffer 指针传给 `uart_control_send()`，DMA 直接从它读，模块不拷贝、不持有发送缓冲。

---

## 2. 缓冲区归属（先理解这个，能少踩坑）

| 方向 | 缓冲归谁 | 原因 |
|------|----------|------|
| RX | **模块持有**（你在 `init` 时提供） | 接收是异步的：数据随时从线上进入，中断/DMA 必须写进一个常驻、始终就绪的缓冲 |
| TX | **应用持有**（发送时传指针） | 发送是你主动发起的，你已把帧组好在自己的 buffer 里，传指针即可 |

**TX 的关键约束**：因为零拷贝，你传入的 `buf` **必须在发送完成之前保持有效**（不能被改写、不能被释放）。发送完成的标志是 `on_tx_done_callback` 被回调、或 `tx_busy` 清零。

---

## 3. 快速上手

### 3.1 定义实例与缓冲

```c
static struct uart_control uart1;

static uint8_t uart1_rx_buf[128];   /* 只需接收缓冲，发送不需要 */
```

### 3.2 适配底层硬件（实现 4 个 ops）

```c
static void uart1_send(const uint8_t *buf, uint16_t len)
{
    /* 启动 UART DMA/中断发送，数据源是 buf */
}

static void uart1_rx_start(void)   { /* 使能 UART 接收 */ }
static void uart1_rx_stop(void)    { /* 关闭 UART 接收 */ }

static uint8_t uart1_read_byte(void)
{
    /* 读 UART 接收寄存器，返回 1 字节 */
    return 0u;
}

static const struct uart_control_hal_ops uart1_ops = {
    .send      = uart1_send,
    .rx_start  = uart1_rx_start,
    .rx_stop   = uart1_rx_stop,
    .read_byte = uart1_read_byte,
};
```

### 3.3 实现回调

```c
static void uart1_rx_done(struct uart_control *uart, uint16_t len)
{
    uint8_t byte;

    if (uart_control_rx_overflow(uart))
    {
        /* 帧超过 rx_buf，数据已被截断，按需丢弃或告警 */
    }

    while (uart_control_rx_read(uart, &byte))
    {
        /* 逐个字节处理这一帧 */
    }

    uart_control_enable_rx(uart);   /* 处理完重新启动接收 */
}

static void uart1_tx_done(struct uart_control *uart)
{
    (void)uart;
    /* 发送完成，此时可以安全复用上一次发送的 buf */
}
```

### 3.4 初始化

```c
void uart1_control_init(void)
{
    if (uart_control_init(&uart1,
                          &uart1_ops,
                          uart1_rx_buf, sizeof(uart1_rx_buf),
                          5u,                 /* timeout_tick，见 §5 */
                          uart1_rx_done,
                          uart1_tx_done) == UART_CONTROL_OK)
    {
        uart_control_enable_rx(&uart1);
    }
}
```

### 3.5 中断接入

```c
void UART_RX_IRQHandler(void)   { uart_control_rx_isr(&uart1); }
void UART_TX_DONE_IRQHandler(void) { uart_control_tx_done_isr(&uart1); }
void TIMER_IRQHandler(void)     { uart_control_timer_isr(&uart1); }
```

### 3.6 发送一帧

```c
uint8_t tx_frame[128];   /* 应用自己的发送帧缓冲 */

/* ... 填充 tx_frame，得到帧长 len ... */

switch (uart_control_send(&uart1, tx_frame, len))
{
case UART_CONTROL_OK:    break;   /* 已启动发送 */
case UART_CONTROL_BUSY:  break;   /* 上一帧还没发完，稍后再试 */
default:                 break;   /* buf == NULL 或 len == 0 */
}

/* 注意：在 uart1_tx_done 回调之前，不要再改写 tx_frame */
```

---

## 4. 主要 API

| 函数 | 说明 |
|------|------|
| `uart_control_init()` | 初始化实例：绑定 ops、RX 缓冲、超时时间、回调 |
| `uart_control_enable_rx()` | 启动一帧接收（清空读写指针、溢出标志） |
| `uart_control_send(uart, buf, len)` | 零拷贝发送一帧，DMA 直接读 `buf` |
| `uart_control_rx_len()` | 当前帧总长度（已接收字节数） |
| `uart_control_rx_remaining()` | 当前帧剩余未读字节数 |
| `uart_control_rx_read()` | 读当前帧下一字节，读完返回 `false` |
| `uart_control_rx_overflow()` | 当前帧是否接收溢出（被截断） |
| `uart_control_bind_rx_done_callback()` | 运行时更换接收完成回调 |
| `uart_control_bind_tx_done_callback()` | 运行时更换发送完成回调 |
| `uart_control_rx_isr()` | 接收中断入口（每收到 1 字节调用一次） |
| `uart_control_tx_done_isr()` | 发送完成中断入口 |
| `uart_control_timer_isr()` | 空闲超时定时器入口（周期调用） |

---

## 5. 接收流程与超时

接收流程：

1. `uart_control_enable_rx()` 启动接收。
2. 每收到 1 字节，在接收中断里调 `uart_control_rx_isr()`，模块把它写入 `rx_buf` 并清零超时计数。
3. 定时器周期性调 `uart_control_timer_isr()`。
4. 连续空闲达到 `timeout_tick` 次后，模块停止接收，回调 `on_rx_done_callback(uart, len)`。
5. 你在回调里读数据，处理完后再次调 `uart_control_enable_rx()` 启动下一帧。

**`timeout_tick` 的单位** = 你调用 `uart_control_timer_isr()` 的周期。例如定时器每 1ms 调一次，`timeout_tick = 5` 表示空闲 5ms 判定一帧结束。注意这个值要小于两帧之间的正常间隔，否则会把两帧误拼成一帧。

---

## 6. 发送流程与零拷贝

发送流程：

1. 你在自己的 buffer 里组好帧。
2. `uart_control_send(uart, buf, len)` 检查后置 `tx_busy = true`，调 `ops->send(buf, len)` 启动 DMA。
3. DMA 完成后，你在发送完成中断里调 `uart_control_tx_done_isr()`，模块清 `tx_busy` 并回调 `on_tx_done_callback`。
4. 回调后你才能安全复用 `buf`。

要点：

- **忙则拒绝**：`tx_busy == true` 时再调 `uart_control_send()` 会返回 `UART_CONTROL_BUSY`，不会启动第二次发送。发送串行化靠它。
- **不拷贝**：模块不把 `buf` 拷到内部，DMA 直接读你的 `buf`，所以发送期间别改它。
- **无长度上限校验**：模块不持有发送缓冲，也不知道它多大，只做 `buf != NULL && len != 0` 检查。`len` 是 `uint16_t`（≤65535），在 DMA 块长度寄存器范围内；传错长度是你自己的 bug。

---

## 7. 返回值

| 返回值 | 含义 |
|--------|------|
| `UART_CONTROL_OK` | 成功 |
| `UART_CONTROL_ERROR` | 参数非法（NULL 指针、len=0 等） |
| `UART_CONTROL_TIMEOUT` | 预留状态，当前未使用 |
| `UART_CONTROL_BUSY` | 正在发送，暂不能启动新发送 |

---

## 8. 常见坑

1. **发送期间改写 `buf`** —— 最常见的坑。零拷贝下 DMA 直接读你的 `buf`，务必等到 `on_tx_done_callback` 再动它。如果上层会"每 100ms 重建一帧而 DMA 要 256ms 才发完"，需要在重建前自己判断发送是否完成（例如加 `tx_done` 标志）。

2. **忘记在 RX 回调后重新 `enable_rx`** —— 处理完一帧若不再调 `uart_control_enable_rx()`，后续数据收不到。

3. **`timeout_tick` 设错** —— 设太小会把一帧拆成多帧，设太大会把多帧并成一帧，还会让响应变慢。

4. **`rx_buf` 太小** —— 帧长超过 `rx_size` 会置 `rx_overflow` 并截断，只保留前 `rx_size` 字节。根据最大帧长留足余量。

5. **ISR 未接入 / 接入顺序错误** —— 三个 ISR 入口（`rx_isr`/`tx_done_isr`/`timer_isr`）都要接，且在 `uart_control_init()` 成功后再调用。

6. **多实例** —— 每个 UART 一个 `struct uart_control` 实例 + 各自的 `rx_buf` + 各自的 ops。回调都带 `uart` 参数，用它区分是哪一路。

---

## 9. 完整示例（串口回显）

收到什么回什么，演示 RX + TX 闭环：

```c
static struct uart_control echo_uart;
static uint8_t echo_rx_buf[128];
static uint8_t echo_tx_buf[128];   /* 应用自己的发送缓冲 */

/* --- 回调 --- */
static void echo_on_rx(struct uart_control *uart, uint16_t len)
{
    uint16_t i = 0;

    if (uart_control_rx_overflow(uart))
        return;                       /* 溢出丢弃 */

    while (i < len && uart_control_rx_read(uart, &echo_tx_buf[i]))
        i++;

    /* 回显：把收到的原样发回 */
    if (uart_control_send(uart, echo_tx_buf, i) != UART_CONTROL_OK)
    {
        /* 忙则丢弃，或缓冲后重试 */
    }

    uart_control_enable_rx(uart);
}

static void echo_on_tx(struct uart_control *uart)
{
    (void)uart;
    /* echo_tx_buf 现在可以复用了 */
}

/* --- ops 与 init 略，见 §3 --- */
```

> 注意上面的 `echo_on_tx` 之前，`echo_tx_buf` 仍被 DMA 占用，不能重复回显。若收到新帧时上次回显还没发完，`uart_control_send` 会返回 `UART_CONTROL_BUSY`，这里选择丢弃；需要可靠的话用环形缓冲 + 在 `echo_on_tx` 里取下一帧补发。
