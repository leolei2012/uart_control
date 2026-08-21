#ifndef UART_CONTROL_H
#define UART_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/** @name 版本号 */
/** @{ */
#define UART_CONTROL_VERSION_MAJOR   1
#define UART_CONTROL_VERSION_MINOR   0
#define UART_CONTROL_VERSION_PATCH   0

/** 版本号字符串（如 "1.0.0"） */
#define UART_CONTROL_VERSION_STRING  "1.0.0"

/** 版本号数值：major<<16 | minor<<8 | patch，用于编译期/运行期比较 */
#define UART_CONTROL_VERSION_NUM     ((UART_CONTROL_VERSION_MAJOR << 16) | (UART_CONTROL_VERSION_MINOR << 8) | UART_CONTROL_VERSION_PATCH)
/** @} */

enum
{
    UART_CONTROL_OK = 0,
    UART_CONTROL_ERROR,
    UART_CONTROL_BUSY
};

/**
 * @brief  通用 UART 收发模块（芯片无关，通过 ops 注入硬件操作）
 *
 * 发送流程：uart_control_send() → ops->send() → 回调 on_tx_done_callback
 * 接收流程：uart_control_enable_rx() → 逐字节收，复位超时定时器
 *         → 定时器超时（帧结束）→ 回调 on_rx_done_callback
 *         uart_control_disable_rx() → 停止接收（关闭 RX 中断）
 *
 * 使用流程：
 *   1. 填充 struct uart_control_hal_ops，绑定硬件操作
 *   2. uart_control_init() —— 绑定接收缓冲区、ops、超时时间和回调
 *   3. uart_control_enable_rx() —— 启动接收
 *   4. uart_control_disable_rx() —— 停止接收（关闭 RX 中断）
 *   5. uart_control_send(buf, len) —— 零拷贝发送（DMA 直接读 buf）
 *   6. ISR 中调用:
 *      - uart_control_rx_isr()       → 收到一个字节
 *      - uart_control_tx_done_isr()  → 发送完成
 *      - uart_control_timer_isr()    → 空闲超时（帧接收完成）
 *
 * 缓冲区归属设计（为什么只有 RX 缓冲，没有 TX 缓冲）：
 *   - 只持有接收缓冲 rx_buf：接收是异步的，数据随时从线上进入，DMA/中断
 *     必须写入一个常驻、始终就绪的缓冲，故该缓冲随实例由调用方在 init 提供。
 *   - 不持有发送缓冲：发送由应用主动发起，应用已把帧组好在自己的 buffer，
 *     发送时把指针传入 uart_control_send(buf, len) 即可，DMA 直接从 buf 读，
 *     省去库内冗余拷贝与 tx_buf 内存。
 *   - 约束：调用方必须保证 buf 在发送完成（on_tx_done_callback / tx_busy
 *     清零）前保持有效，DMA 期间不得改写。
 */

/**
 * @brief  硬件操作抽象
 *
 * 调用方注入具体 HAL 实现，使模块与芯片解耦。
 */
struct uart_control_hal_ops
{
    /** @brief 发送一帧数据，成功启动返回 UART_CONTROL_OK，失败返回 UART_CONTROL_ERROR */
    uint8_t (*send)(const uint8_t *buf, uint16_t len);

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

    uint16_t timeout_tick;                   /**< 空闲超时时间 (ticks) */
    volatile uint16_t timeout_tick_cnt;      /**< 空闲超时计数器 (ticks) */

    uint16_t rx_rd;                          /**< 读指针 */

    volatile bool rx_active;                 /**< 接收进行中 */
    volatile bool tx_busy;                   /**< 发送进行中 */
    volatile bool rx_overflow;               /**< 接收缓冲区溢出（帧不完整） */

    void *ctx;                               /**< 用户上下文（回调里通过 self->ctx 取回） */

    void (*on_rx_done_callback)(struct uart_control *self, uint16_t len); /**< 帧接收完成回调 */
    void (*on_tx_done_callback)(struct uart_control *self);                /**< 发送完成回调 */
};

/**
 * @name   API
 * @{
 */

/**
 * @brief  初始化 UART 实例
 *
 * 绑定硬件操作 ops、接收缓冲区、空闲超时时间及完成回调。
 * 调用后需手动 uart_control_enable_rx 启动接收。
 *
 * @param timeout_tick  空闲超时时间 (ticks)，典型值 5~10
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法
 */
uint8_t uart_control_init(struct uart_control               *self,
                          const struct uart_control_hal_ops *ops,
                          uint8_t                           *rx_buf,
                          uint16_t                           rx_size,
                          uint16_t                           timeout_tick,
                          void (*on_rx_done_callback)(struct uart_control *self, uint16_t len),
                          void (*on_tx_done_callback)(struct uart_control *self));

/**
 * @brief  启动一帧接收
 *
 * 清空读写指针、超时计数器及溢出标志，使能字节接收。
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法
 */
uint8_t uart_control_enable_rx(struct uart_control *self);

/**
 * @brief  停止接收（关闭 RX 相关中断）
 *
 * 调用 ops->rx_stop 关闭 RX 中断并置 rx_active = false。
 */
void uart_control_disable_rx(struct uart_control *self);

/**
 * @brief  发送一帧数据（零拷贝：DMA 直接从调用方缓冲 buf 读取）
 *
 * 调用 ops->send 启动发送，发送完成后回调 on_tx_done_callback。
 * ops->send 返回非 UART_CONTROL_OK 时视为启动失败，tx_busy 会被回滚并返回 UART_CONTROL_ERROR。
 * 调用方需保证 buf 在发送完成（on_tx_done_callback / tx_busy 清零）前保持有效。
 * @param buf  待发送数据指针
 * @param len  发送字节数
 * @return UART_CONTROL_OK 成功，UART_CONTROL_ERROR 参数非法或启动失败，UART_CONTROL_BUSY 发送忙
 */
uint8_t uart_control_send(struct uart_control *self, const uint8_t *buf, uint16_t len);

void uart_control_bind_rx_done_callback(struct uart_control *self, void (*callback)(struct uart_control *self, uint16_t len));
void uart_control_bind_tx_done_callback(struct uart_control *self, void (*callback)(struct uart_control *self));

/** @brief 设置用户上下文（回调里通过 self->ctx 取回，用于多实例） */
void uart_control_set_ctx(struct uart_control *self, void *ctx);

/** @brief 获取当前帧总长度（已接收字节数） */
uint16_t uart_control_rx_len(struct uart_control *self);

/** @brief 获取当前帧剩余未读字节数 */
uint16_t uart_control_rx_remaining(struct uart_control *self);

/** @brief 从 RX 缓冲区读取一个字节，成功返回 true，无数据返回 false */
bool     uart_control_rx_read(struct uart_control *self, uint8_t *byte);

/** @brief 查询当前帧是否发生缓冲区溢出 */
bool     uart_control_rx_overflow(struct uart_control *self);

/** @} */

/**
 * @name   ISR 入口（bsp_it.c 中调用）
 * @{
 */

void uart_control_rx_isr(struct uart_control *self);       /**< 收到字节 */
void uart_control_tx_done_isr(struct uart_control *self);  /**< 发送完成 */
void uart_control_timer_isr(struct uart_control *self);    /**< 空闲超时 */

/** @} */

#endif /* UART_CONTROL_H */
