#include "uart_control.h"

#include <string.h>

/**
 * @brief  初始化 UART 实例
 *
 * 绑定硬件操作 ops、接收缓冲区、空闲超时时间及完成回调。
 * 调用后需手动 uart_control_enable_rx 启动接收。
 *
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法
 */
uint8_t uart_control_init(struct uart_control               *self,
                          const struct uart_control_hal_ops *ops,
                          uint8_t                           *rx_buf,
                          uint16_t                           rx_size,
                          uint16_t                           timeout_tick,
                          void (*on_rx_done_callback)(struct uart_control *self, uint16_t len),
                          void (*on_tx_done_callback)(struct uart_control *self))
{
    if (self == NULL || ops == NULL || rx_buf == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    if (ops->send == NULL || ops->rx_start == NULL ||
        ops->rx_stop == NULL || ops->read_byte == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    if (rx_size == 0u || timeout_tick == 0u)
    {
        return UART_CONTROL_ERROR;
    }

    memset(self, 0, sizeof(*self));

    self->ops = ops;
    self->rx_buf = rx_buf;
    self->rx_size = rx_size;
    self->timeout_tick = timeout_tick;
    self->on_rx_done_callback = on_rx_done_callback;
    self->on_tx_done_callback = on_tx_done_callback;

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
uint8_t uart_control_enable_rx(struct uart_control *self)
{
    if (self == NULL || self->ops == NULL || self->ops->rx_start == NULL || self->rx_buf == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    self->rx_wr = 0u;
    self->rx_rd = 0u;
    self->timeout_tick_cnt = 0u;
    self->rx_overflow = false;
    self->rx_active = true;

    self->ops->rx_start();

    return UART_CONTROL_OK;
}

/**
 * @brief  停止接收（关闭 RX 相关中断）
 *
 * 调用 ops->rx_stop 关闭 RX 中断并置 rx_active = false。
 */
void uart_control_disable_rx(struct uart_control *self)
{
    if ((self == NULL) || (self->ops == NULL) || (self->ops->rx_stop == NULL))
    {
        return;
    }

    self->ops->rx_stop();
    self->rx_active = false;
}

/**
 * @brief  发送一帧数据（零拷贝：DMA 直接从调用方缓冲 buf 读取）
 *
 * 调用 ops->send 启动发送，发送完成后回调 on_tx_done_callback。
 * 调用方需保证 buf 在发送完成（on_tx_done_callback / tx_busy 清零）前保持有效。
 *
 * @param buf  待发送数据指针
 * @param len  发送字节数
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法，UART_CONTROL_BUSY 发送忙
 */
uint8_t uart_control_send(struct uart_control *self, const uint8_t *buf, uint16_t len)
{
    if (self == NULL || buf == NULL || self->ops == NULL || self->ops->send == NULL)
    {
        return UART_CONTROL_ERROR;
    }

    if (len == 0u)
    {
        return UART_CONTROL_ERROR;
    }

    if (self->tx_busy)
    {
        return UART_CONTROL_BUSY;
    }

    self->tx_busy = true;

    if (self->ops->send(buf, len) != UART_CONTROL_OK)
    {
        self->tx_busy = false;
        return UART_CONTROL_ERROR;
    }

    return UART_CONTROL_OK;
}

/**
 * @brief  获取当前帧总长度（已接收字节数）
 *
 * 在 on_rx_done_callback 回调中调用，返回值即帧总长度。
 */
uint16_t uart_control_rx_len(struct uart_control *self)
{
    if (self == NULL)
    {
        return 0u;
    }

    return self->rx_wr;
}

/**
 * @brief  获取当前帧剩余未读字节数
 *
 * 配合 uart_control_rx_read 使用逐字节读取。
 */
uint16_t uart_control_rx_remaining(struct uart_control *self)
{
    if (self == NULL)
    {
        return 0u;
    }

    return self->rx_wr - self->rx_rd;
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
bool uart_control_rx_read(struct uart_control *self, uint8_t *byte)
{
    if (self == NULL || self->rx_buf == NULL || byte == NULL)
    {
        return false;
    }

    if (self->rx_rd < self->rx_wr)
    {
        *byte = self->rx_buf[self->rx_rd];
        self->rx_rd++;
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
bool uart_control_rx_overflow(struct uart_control *self)
{
    if (self == NULL)
    {
        return false;
    }

    return self->rx_overflow;
}

/**
 * @brief  绑定帧接收完成回调
 *
 * 可运行时更换回调，不影响接收状态。
 */
void uart_control_bind_rx_done_callback(struct uart_control *self, void (*callback)(struct uart_control *self, uint16_t len))
{
    if (self == NULL)
    {
        return;
    }

    self->on_rx_done_callback = callback;
}

/**
 * @brief  绑定发送完成回调
 */
void uart_control_bind_tx_done_callback(struct uart_control *self, void (*callback)(struct uart_control *self))
{
    if (self == NULL)
    {
        return;
    }

    self->on_tx_done_callback = callback;
}

/**
 * @brief  设置用户上下文
 *
 * 回调里通过 self->ctx 取回，用于多实例场景（避免调用方再维护全局单例）。
 */
void uart_control_set_ctx(struct uart_control *self, void *ctx)
{
    if (self == NULL)
    {
        return;
    }

    self->ctx = ctx;
}

/**
 * @brief  ISR: 收到一个字节
 *
 * 由接收中断调用。
 * 先判断 rx_active，再读硬件字节，避免在接收未使能时消耗硬件 FIFO。
 * 缓冲区满时刷新超时计数器并标记溢出，保证帧结束回调仍能触发。
 */
void uart_control_rx_isr(struct uart_control *self)
{
    if (self == NULL || self->ops == NULL || self->ops->read_byte == NULL)
    {
        return;
    }

    if (!self->rx_active)
    {
        return;
    }

    uint8_t byte = self->ops->read_byte();

    if (self->rx_wr < self->rx_size)
    {
        self->rx_buf[self->rx_wr] = byte;
        self->rx_wr++;
    }
    else
    {
        self->rx_overflow = true;
    }

    self->timeout_tick_cnt = 0u;
}

/**
 * @brief  ISR: 发送完成
 *
 * 由发送完成中断调用，清除发送忙标志后回调 on_tx_done_callback 通知应用层。
 */
void uart_control_tx_done_isr(struct uart_control *self)
{
    if (self == NULL)
    {
        return;
    }

    self->tx_busy = false;

    if (self->on_tx_done_callback != NULL)
    {
        self->on_tx_done_callback(self);
    }
}

/**
 * @brief  ISR: 空闲超时，一帧接收完成
 *
 * 由空闲定时器中断调用。停止接收，回调 on_rx_done_callback(self, len)。
 * 若超时时缓冲区为空则忽略。
 */
void uart_control_timer_isr(struct uart_control *self)
{
    if (self == NULL || self->ops == NULL || self->ops->rx_stop == NULL)
    {
        return;
    }

    if (!self->rx_active)
    {
        return;
    }

    if (self->rx_wr == 0u)
    {
        return;
    }

    self->timeout_tick_cnt++;

    if (self->timeout_tick_cnt >= self->timeout_tick)
    {
        uint16_t len;

        self->ops->rx_stop();
        self->rx_active = false;
        len = self->rx_wr;

        if (self->on_rx_done_callback != NULL)
        {
            self->on_rx_done_callback(self, len);
        }
    }
}
