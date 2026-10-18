# Phase 2 实施计划：驱动与 Web 加固（AT 延后）

> 目标：在**不增加新功能**的前提下，把 Phase 1 已有功能做"基本成熟"：稳定、安全、可恢复。
> AT 诊断属于新功能，按用户要求放在 Phase 2.1/2.2 完成后单独实施。
>
> 依据：`C:\Users\fytech\Desktop\ESP-CPE\AUDIT_REPORT.md` 第七章 P2/P3 清单、第八章 8.2；
> 结合 Phase 1 已完成的修补，只列**尚未完成或需要复核**的项。

## 0. 范围与非目标

**范围**

1. 驱动/枚举健壮性（CDC/ECM 本地 fork、USB Host、main 链路管理）；
2. Web 管理面加固（请求解析、绑定网口、会话/限速、响应安全）；
3. 无 RESET 线条件下的恢复策略收口与验收；
4. 构建卫生、测试脚本、文档。

**非目标（Phase 3）**

- OTA/回滚、Secure Boot、Flash/NVS 加密、HTTPS；
- AT 诊断、公网 IP/测速等新功能；
- 模组 RESET 硬件接线（当前硬件无法实现，仅保留软件接口）。

## 1. 工作包与验收

### P2-1 驱动与枚举加固（优先）

| # | 工作项 | 依据 | 验收 |
| --- | --- | --- | --- |
| 1.1 | 描述符解析去掉 `itf+1` / IAD 固定 2 接口假设，按 Union/EC 功能描述符定位数据接口 | 6.5、7.2 | 用当前模组 + 至少一种不同接口布局的模拟描述符验证 |
| 1.2 | NEW_DEV：descriptor 先拷贝再 `device_close`；DEV_GONE 的 `device_info` 初始化；端口链表遍历加临界区 | C14、7.2 | 热插拔 50 次无悬空指针/panic |
| 1.3 | 端口句柄引用计数，消除 `is_port_opened()` 与使用之间的 TOCTOU | C2、7.2 | 并发 close/open 压测无 UAF |
| 1.4 | IN 重试计数改为 per-port；notification 失败后重新提交；OUT 错误恢复不丢包、检查 `endpoint_clear` 返回值 | C11、C12 | 拔插/错误注入后数据通路可恢复 |
| 1.5 | ECM：通知长度校验、`wMaxSegmentSize` 上限钳制、跨任务状态加锁、`deinit` 幂等 | 7.2 | 反复 deinit/init 不崩、不泄漏 |
| 1.6 | `espressif__usb`：client 事件队列满丢 NEW_DEV 的兜底、控制传输超时、`usb_host_uninstall` 返回值 | L-2/L-3/H-1 | 事件队列压满后仍能枚举；无传输永久阻塞 |
| 1.7 | `main`：`set_default_netif` 提前到 netif 创建时；`iot_eth_install` 失败路径泄漏处理 | 7.1 | 审查 + 失败注入验证 |

### P2-2 Web 管理面加固

| # | 工作项 | 依据 | 验收 |
| --- | --- | --- | --- |
| 2.1 | 请求体读取抽公共函数：长度上限、NUL 终止、分片拼接、Content-Type 校验（替换 4 处复制实现） | 7.4 | 超长/分片/无 Content-Type/畸形 JSON 均返回 400/413，不越界 |
| 2.2 | 全量核查 JSON 响应转义（动态字段统一 `sbuf_append_json_string`），禁止手工拼接 | 7.4 | 字段含引号/换行/截断时 JSON 仍合法 |
| 2.3 | **httpd 只绑定 SoftAP 网口**（`httpd_config_t.if_name`），4G WAN 侧不可访问管理面 | 审计待确认 #6 | 从模组网段访问 `192.168.225.x:80` 被拒绝 |
| 2.4 | 登录限速/退避（现有 5 次锁定 60s，补指数退避）、会话空闲超时、改密/恢复出厂踢掉所有会话 | P2 | 暴力登录被节流；空闲超时后需重新登录 |
| 2.5 | 所有 POST handler 确保 `json_parse_end()`；网络 IO 零 `ESP_ERROR_CHECK` 复核 | R-13、P2 | 长跑请求无 token 泄漏；客户端断连不 abort |
| 2.6 | 静态文件路径规范化、handler 注册返回值检查、`max_uri_handlers`/httpd 栈复核 | P3 | 目录穿越/畸形 URL 返回 404 |

### P2-3 无 RESET 线的恢复策略收口

| # | 工作项 | 验收 |
| --- | --- | --- |
| 3.1 | 枚举失败早期识别：no-device 超时后立即进入 USB Host 栈重启路径，不再多轮 ECM 重建 | 复位后恢复时间可控 |
| 3.2 | 明确"栈重启后仍失败 → FAULT"的上限与退避，绝不重启整机 | 无设备/无 SIM 观察 10 分钟不重启 |
| 3.3 | 文档记录硬件限制；保留 `CPE_MODULE_RESET_GPIO` 代码供后续接线 | README/RUNTIME_FLOW 更新 |

### P2-4 工程与测试

| # | 工作项 | 验收 |
| --- | --- | --- |
| 4.1 | fork 版本/CHANGELOG、`dependencies.lock` 策略、`espressif__usb` 是否 fork 的结论 | 构建可复现 |
| 4.2 | 清理未使用依赖、Kconfig/README 不一致、死代码（P3） | 构建 0 warning |
| 4.3 | 新增测试脚本：热插拔日志统计、Web DoS/XSS/CSRF 回归 | 脚本可重复执行 |
| 4.4 | `docs/PHASE2_TEST_PLAN.md` 记录每项结果 | 验收表完整 |

## 2. 顺序与里程碑

| 里程碑 | 内容 | 状态 |
| --- | --- | --- |
| M2.1 | Web 加固：绑定 AP 网口、请求体公共函数、会话/限速 | 待开始 |
| M2.2 | 驱动加固第一批：ECM deinit/通知长度、per-port IN 重试、NEW_DEV 生命周期 | 待开始 |
| M2.3 | 驱动加固第二批：Union/EC 描述符、usb 栈事件/超时、TOCTOU refcount | 待开始 |
| M2.4 | 无 RESET 线恢复收口 + 上板回归 | 待开始 |
| M2.5 | 构建卫生、测试脚本、Phase 2 测试报告 | 待开始 |
| M2.6 | AT 诊断与状态页（Phase 2 完成后单独立项） | 待开始 |

## 3. 风险与约束

1. **无 RESET 线**：外部供电模组在 ESP32 复位后可能进入 `CHECK_SHORT_DEV_DESC FAILED`，软件只能靠 USB Host 栈重启兜底；上板验收按 FAULT 可接受，文档记录硬件限制。
2. **描述符布局差异**：Union/EC 解析需要模组多布局样本；先用当前模组 + 构造样本验证，后续现场模组再回归。
3. **usb managed 组件**：修改 `espressif__usb` 需决定 fork 或升级，可能影响 OTA 分区外的东西；优先用配置/兜底，不轻易 fork。
4. **AT 新功能**：本阶段不做；等基础功能稳定后再评估模组是否暴露 AT 接口。
5. 每批改动仍遵循 `AGENTS.md`：编译验证前先询问由谁构建。

## 4. 变更记录

| 日期 | 内容 | 提交 |
| --- | --- | --- |
| 2026-10-18 | 建立 Phase 2 计划（驱动 + Web 先行，AT 延后） | 待提交 |
