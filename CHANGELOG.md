# Changelog

本文件记录项目的所有变更，按时间倒序排列。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [Semantic Versioning](https://semver.org/lang/zh-CN/)。

---

## [Unreleased]

### Changed
- **零拷贝发送，移除模块内部 TX 缓冲**：`uart_control` 不再持有 `tx_buf`/`tx_size`，`uart_control_send()` 改为接收调用方 `buf` 指针直接 DMA 发送，`uart_control_init()` 去掉 `tx_buf`/`tx_size` 参数
  - **设计原因**：接收是异步的（数据随时进入，必须常驻缓冲），故保留 `rx_buf`；发送由应用主动发起，应用已组好帧，传指针即可，无需库内冗余拷贝
  - **调用方约束**：传入的 `buf` 必须在发送完成（`on_tx_done_callback` / `tx_busy` 清零）前保持有效
  - **影响**：`uart_control_send()` / `uart_control_init()` 为破坏性 API 变更，所有调用方需同步适配
