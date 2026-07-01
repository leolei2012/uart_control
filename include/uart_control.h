#ifndef UART_CONTROL_H
#define UART_CONTROL_H

#include "platform.h"

enum
{
    UART_CONTROL_OK = 0,
    UART_CONTROL_ERROR,
    UART_CONTROL_TIMEOUT,
    UART_CONTROL_BUSY
};

enum
{
    UART_CONTROL_OFF = 0,
    UART_CONTROL_ON,
};

/**
 * @brief  通用 UART 收发模块（芯片无关，通过 ops 注入硬件操作）
 *
 * 发送流程：uart_control_send() → ops->send() → 回调 on_tx_done_callback
 * 接收流程：uart_control_enable_rx() → 逐字节收，复位超时定时器
 *         → 定时器超时（帧结束）→ 回调 on_rx_done_callback
 *
 * 使用流程：
 *   1. 填充 struct uart_control_hal_ops，绑定硬件操作
 *   2. uart_control_init() —— 绑定缓冲区、ops、超时时间和回调
 *   3. uart_control_enable_rx() —— 启动接收
 *   4. uart_control_send(len) —— 发送数据
 *   5. ISR 中调用:
 *      - uart_control_rx_isr()       → 收到一个字节
 *      - uart_control_tx_done_isr()  → 发送完成
 *      - uart_control_timer_isr()    → 空闲超时（帧接收完成）
 */

/**
 * @brief  硬件操作抽象
 *
 * 调用方注入具体 HAL 实现，使模块与芯片解耦。
 */
struct uart_control_hal_ops
{
    /** @brief 发送一帧数据 */
    void (*send)(const uint8_t *buf, uint16_t len);

    /** @brief 启动逐字节接收 */
    void     (*rx_start)(void);
    /** @brief 停止接收 */
    void     (*rx_stop)(void);
    /** @brief 读取一个接收字节 */
    uint8_t  (*read_byte)(void);
};

/**
 * @brief  UART 实例
 */
struct uart_control
{
    const struct uart_control_hal_ops *ops;  /**< 硬件操作 */

    uint8_t *rx_buf;                         /**< 接收缓冲区 */
    uint16_t rx_size;                        /**< 接收缓冲区大小 */
    volatile uint16_t rx_wr;                 /**< 当前帧已接收字节数 */

    uint8_t *tx_buf;                         /**< 发送缓冲区 */
    uint16_t tx_size;                        /**< 发送缓冲区大小 */

    uint16_t timeout_tick;                   /**< 空闲超时时间 (ticks) */
    volatile uint16_t timeout_tick_cnt;      /**< 空闲超时计数器 (ticks) */

    uint16_t rx_rd;                          /**< 读指针 */

    volatile bool rx_active;                 /**< 接收进行中 */
    volatile bool tx_busy;                   /**< 发送进行中 */
    volatile bool rx_overflow;               /**< 接收缓冲区溢出（帧不完整） */

    void (*on_rx_done_callback)(struct uart_control *uart, uint16_t len); /**< 帧接收完成回调 */
    void (*on_tx_done_callback)(struct uart_control *uart);                /**< 发送完成回调 */
};

/**
 * @name   API
 * @{
 */

/**
 * @brief  初始化 UART 实例
 *
 * 绑定硬件操作 ops、收发缓冲区、空闲超时时间及完成回调。
 * 调用后需手动 uart_control_enable_rx 启动接收。
 *
 * @param timeout_tick  空闲超时时间 (ms)，典型值 5~10
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
                          void (*on_tx_done_callback)(struct uart_control *uart));

/**
 * @brief  启动一帧接收
 *
 * 清空读写指针、超时计数器及溢出标志，使能字节接收。
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法
 */
uint8_t uart_control_enable_rx(struct uart_control *uart);

/**
 * @brief  发送一帧数据
 *
 * 调用 ops->send 启动发送，发送完成后回调 on_tx_done_callback。
 * @param len  发送字节数（≤ tx_size）
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法，UART_CONTROL_BUSY 发送忙
 */
uint8_t uart_control_send(struct uart_control *uart, uint16_t len);

void uart_control_bind_rx_done_callback(struct uart_control *uart, void (*callback)(struct uart_control *uart, uint16_t len));
void uart_control_bind_tx_done_callback(struct uart_control *uart, void (*callback)(struct uart_control *uart));

/** @brief 获取当前帧总长度（已接收字节数） */
uint16_t uart_control_rx_len(struct uart_control *uart);

/** @brief 获取当前帧剩余未读字节数 */
uint16_t uart_control_rx_remaining(struct uart_control *uart);

/** @brief 从 RX 缓冲区读取一个字节，成功返回 true，无数据返回 false */
bool     uart_control_rx_read(struct uart_control *uart, uint8_t *byte);

/** @brief 查询当前帧是否发生缓冲区溢出 */
bool     uart_control_rx_overflow(struct uart_control *uart);

/** @} */

/**
 * @name   ISR 入口（bsp_it.c 中调用）
 * @{
 */

void uart_control_rx_isr(struct uart_control *uart);       /**< 收到字节 */
void uart_control_tx_done_isr(struct uart_control *uart);  /**< 发送完成 */
void uart_control_timer_isr(struct uart_control *uart);    /**< 空闲超时 */

/** @} */

#endif /* UART_CONTROL_H */
