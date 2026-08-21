# Changelog

本文件记录项目的所有变更，按时间倒序排列。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

---

## [1.0.0] - 2026-08-22

### Changed
- **增加用户上下文 `ctx`**：`struct uart_control` 增加 `void *ctx` 字段，新增 `uart_control_set_ctx()`；回调里通过 `self->ctx` 取回实例（回调签名不变，支持多实例）。
- **对象指针参数统一命名 `self`**：原 `uart` 全部改为 `self`（纯命名重构，不影响 ABI）。
- **零拷贝发送，移除模块内部 TX 缓冲**：`uart_control` 不再持有 `tx_buf`/`tx_size`，`uart_control_send()` 改为接收调用方 `buf` 指针直接 DMA 发送，`uart_control_init()` 去掉 `tx_buf`/`tx_size` 参数
  - **设计原因**：接收是异步的（数据随时进入，必须常驻缓冲），故保留 `rx_buf`；发送由应用主动发起，应用已组好帧，传指针即可，无需库内冗余拷贝
  - **调用方约束**：传入的 `buf` 必须在发送完成（`on_tx_done_callback` / `tx_busy` 清零）前保持有效
  - **影响**：`uart_control_send()` / `uart_control_init()` 为破坏性 API 变更，所有调用方需同步适配

- **`ops->send` 返回类型改为 `uint8_t`**：`uart_control_send()` 可感知底层发送是否成功启动，启动失败时回滚 `tx_busy` 并返回 `UART_CONTROL_ERROR`
  - **影响**：`struct uart_control_hal_ops::send` 为破坏性 API 变更，所有 ops 适配需改为返回 `UART_CONTROL_OK` / `UART_CONTROL_ERROR`

### Removed
- 删除未使用的 `UART_CONTROL_TIMEOUT`、`UART_CONTROL_OFF`、`UART_CONTROL_ON` 枚举值

### Fixed
- **补齐 `<string.h>`**：`uart_control_init()` 中的 `memset()` 此前缺少声明，修复编译错误
- **`uart_control_timer_isr()` 帧长度竞态**：将 `len` 的读取移到 `ops->rx_stop()` 之后，避免停收前新到字节导致回调长度漏报
- **参数判空补齐**：ISR 入口（`rx_isr` / `tx_done_isr` / `timer_isr`）及 `uart_control_send()` / `uart_control_enable_rx()` 增加 NULL 与函数指针校验
- **文档单位错误**：`uart_control_init()` 的 `timeout_tick` 注释由 (ms) 修正为 (ticks)
