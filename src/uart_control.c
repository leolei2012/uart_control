#include "uart_control.h"

/**
 * @brief  初始化 UART 实例
 *
 * 绑定硬件操作 ops、收发缓冲区、空闲超时时间及完成回调。
 * 调用后需手动 uart_control_enable_rx 启动接收。
 *
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法
 */
uint8_t uart_control_init(struct uart_control               *uart,
                          const struct uart_control_hal_ops *ops,
                          uint8_t                           *rx_buf,
                          uint16_t                           rx_size,
                          uint8_t                           *tx_buf,
                          uint16_t                           tx_size,
                          uint16_t                           timeout_tick,
                          void (*on_rx_done_callback)(struct uart_control *uart, uint16_t len),
                          void (*on_tx_done_callback)(struct uart_control *uart))
{
    if (uart == NULL || ops == NULL || rx_buf == NULL || tx_buf == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    if (ops->send == NULL || ops->rx_start == NULL ||
        ops->rx_stop == NULL || ops->read_byte == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    if (rx_size == 0u || tx_size == 0u || timeout_tick == 0u)
    {
        return UART_CONTROL_ERROR;
    }

    memset(uart, 0, sizeof(*uart));

    uart->ops = ops;
    uart->rx_buf = rx_buf;
    uart->rx_size = rx_size;
    uart->tx_buf = tx_buf;
    uart->tx_size = tx_size;
    uart->timeout_tick = timeout_tick;
    uart->on_rx_done_callback = on_rx_done_callback;
    uart->on_tx_done_callback = on_tx_done_callback;

    return UART_CONTROL_OK;
}

/**
 * @brief  启动一帧接收
 *
 * 清空读写指针、超时计数器及溢出标志，使能字节接收。
 * 每收到一个字节由 uart_control_rx_isr 写入缓冲区。
 *
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法
 */
uint8_t uart_control_enable_rx(struct uart_control *uart)
{
    if (uart == NULL || uart->ops == NULL || uart->rx_buf == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    uart->rx_wr = 0u;
    uart->rx_rd = 0u;
    uart->timeout_tick_cnt = 0u;
    uart->rx_overflow = false;
    uart->rx_active = true;

    uart->ops->rx_start();

    return UART_CONTROL_OK;
}

/**
 * @brief  发送一帧数据
 *
 * 调用 ops->send 启动发送，发送完成后回调 on_tx_done_callback。
 *
 * @param len  发送字节数（≤ tx_size）
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法，UART_CONTROL_BUSY 发送忙
 */
uint8_t uart_control_send(struct uart_control *uart, uint16_t len)
{
    if (uart == NULL || uart->tx_buf == NULL || uart->ops == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    if (len == 0u || len > uart->tx_size)
    {
        return UART_CONTROL_ERROR;
    }

    if (uart->tx_busy)
    {
        return UART_CONTROL_BUSY;
    }

    uart->tx_busy = true;
    uart->ops->send(uart->tx_buf, len);

    return UART_CONTROL_OK;
}

/**
 * @brief  获取当前帧总长度（已接收字节数）
 *
 * 在 on_rx_done_callback 回调中调用，返回值即帧总长度。
 */
uint16_t uart_control_rx_len(struct uart_control *uart)
{
    if (uart == NULL)
    {
        return 0u;
    }

    return uart->rx_wr;
}

/**
 * @brief  获取当前帧剩余未读字节数
 *
 * 配合 uart_control_rx_read 使用逐字节读取。
 */
uint16_t uart_control_rx_remaining(struct uart_control *uart)
{
    if (uart == NULL)
    {
        return 0u;
    }

    return uart->rx_wr - uart->rx_rd;
}

/**
 * @brief  从 RX 缓冲区读取一个字节
 *
 * 每次调用读取下一个字节，读完后 rx_rd 保持等于 rx_wr，后续读取返回 false。
 * 下一帧开始时由 uart_control_enable_rx() 统一复位读指针。
 *
 * @param byte  输出参数，接收读取的字节
 * @return true 读取成功，false 无数据可读
 */
bool uart_control_rx_read(struct uart_control *uart, uint8_t *byte)
{
    if (uart == NULL || uart->rx_buf == NULL || byte == NULL)
    {
        return false;
    }

    if (uart->rx_rd < uart->rx_wr)
    {
        *byte = uart->rx_buf[uart->rx_rd];
        uart->rx_rd++;
        return true;
    }

    return false;
}

/**
 * @brief  查询当前帧是否发生缓冲区溢出
 *
 * 若返回 true，说明帧长超过 rx_size，数据被截断，帧不完整。
 * 应在 on_rx_done_callback 中检查。
 */
bool uart_control_rx_overflow(struct uart_control *uart)
{
    if (uart == NULL)
    {
        return false;
    }

    return uart->rx_overflow;
}

/**
 * @brief  绑定帧接收完成回调
 *
 * 可运行时更换回调，不影响接收状态。
 */
void uart_control_bind_rx_done_callback(struct uart_control *uart, void (*callback)(struct uart_control *uart, uint16_t len))
{
    if (uart == NULL)
    {
        return;
    }

    uart->on_rx_done_callback = callback;
}

/**
 * @brief  绑定发送完成回调
 */
void uart_control_bind_tx_done_callback(struct uart_control *uart, void (*callback)(struct uart_control *uart))
{
    if (uart == NULL)
    {
        return;
    }

    uart->on_tx_done_callback = callback;
}

/**
 * @brief  ISR: 收到一个字节
 *
 * 由接收中断调用。
 * 先判断 rx_active，再读硬件字节，避免在接收未使能时消耗硬件 FIFO。
 * 缓冲区满时刷新超时计数器并标记溢出，保证帧结束回调仍能触发。
 */
void uart_control_rx_isr(struct uart_control *uart)
{
    if (!uart->rx_active)
    {
        return;
    }

    uint8_t byte = uart->ops->read_byte();

    if (uart->rx_wr < uart->rx_size)
    {
        uart->rx_buf[uart->rx_wr] = byte;
        uart->rx_wr++;
    }
    else
    {
        uart->rx_overflow = true;
    }

    uart->timeout_tick_cnt = 0u;
}

/**
 * @brief  ISR: 发送完成
 *
 * 由发送完成中断调用，清除发送忙标志后回调 on_tx_done_callback 通知应用层。
 */
void uart_control_tx_done_isr(struct uart_control *uart)
{
    uart->tx_busy = false;

    if (uart->on_tx_done_callback != NULL)
    {
        uart->on_tx_done_callback(uart);
    }
}

/**
 * @brief  ISR: 空闲超时，一帧接收完成
 *
 * 由空闲定时器中断调用。停止接收，回调 on_rx_done_callback(uart, len)。
 * 若超时时缓冲区为空则忽略。
 */
void uart_control_timer_isr(struct uart_control *uart)
{
    if (!uart->rx_active)
    {
        return;
    }

    uint16_t len = uart->rx_wr;

    if (len > 0u)
    {
        uart->timeout_tick_cnt++;

        if (uart->timeout_tick_cnt >= uart->timeout_tick)
        {
            uart->ops->rx_stop();
            uart->rx_active = false;

            if (uart->on_rx_done_callback != NULL)
            {
                uart->on_rx_done_callback(uart, len);
            }
        }
    }
}
