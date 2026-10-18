# Phase 3 计划：产品化（OTA / 安全 / 可靠性 / 出厂）

> 前置：Phase 2 已收口（见 `PHASE2_TEST_PLAN.md` 第 6 节）。
> 约束：ESP32-S3 / 16MB Flash / 双 OTA 分区（`partitions_16m_ota.csv`）/ SoftAP 管理面。
> 原则：每批改动 `idf.py build` 通过（构建前先按 `AGENTS.md` 询问由谁构建）后再继续。

## 1. 目标与非目标

目标（Phase 3 完成定义）：

1. 可通过管理面**本地升级固件**（浏览器上传），失败可回滚，断电不砖；
2. 管理面与凭据达到可交付的安全基线（HTTPS/Secure Cookie、口令哈希强度、会话策略）；
3. 具备长稳与故障可诊断能力（看门狗、日志分级、故障码、长稳脚本）；
4. 出厂流程可批量执行（设备标签/二维码、批量测试、恢复出厂路径完整）。

非目标（留待 Phase 4 或硬件改版）：

- 4G 模组的 USB 挂死问题（见 `PHASE2_TEST_PLAN.md` 2.w）——需换模组或硬件增加 `RESET_N`/受控供电；
- AT 指令诊断：当前模组为纯 ECM（无 AT 依赖），等确认模组暴露 AT CDC-ACM 接口后再立项；
- 云端管理平台/远程升级通道（本地 OTA 先行，URL/HTTPS OTA 视网络形态再定）。

## 2. 里程碑

| 里程碑 | 内容 | 验收 |
| --- | --- | --- |
| P3-1 固件升级（OTA） | 管理面本地升级：`/api/ota/status`、`/api/ota/upload`、`/api/system/reboot`；`esp_ota_*` 流式写入；启用 `BOOTLOADER_APP_ROLLBACK_ENABLE`；启动健康后 mark-valid；Web 升级页面（进度/版本/确认） | 上传 → 自动重启 → 新版本运行 → 120s 健康后不回滚；中断/超大/非固件包被拒绝且旧版本可用 |
| P3-2 安全加固 | HTTPS（自签证书）或至少 Secure Cookie + 明确 HTTP 风险提示；PBKDF2 迭代次数上调（兼容旧哈希升级）；会话并发/空闲策略复核；Secure Boot / Flash 加密 / NVS 加密的量产选项评估与文档 | 浏览器 HTTPS 访问无告警可接受方案；旧口令哈希自动升级；安全评估文档化 |
| P3-3 可靠性 | 任务看门狗（链路管理器/Web）、日志分级与敏感信息脱敏、故障码（FAULT 原因枚举）、长稳脚本（复位×N、热插拔×N、无 SIM×N、Web 压测） | 看门狗可触发且不清空恢复计数策略；长稳脚本可无人值守跑通并出报告 |
| P3-4 出厂与运维 | 设备标签/二维码内容（AP SSID/口令、管理地址、序列号）；批量测试工具（串口 + Web）；序列号/生产日期写入 NVS；恢复出厂（含 Web、GPIO 长按、串口命令） | 一台设备 3 分钟内完成出厂检测并生成记录 |

## 3. P3-1 OTA 详细设计

### 3.1 分区与回滚

- 现有 `partitions_16m_ota.csv`：`ota_0` 0x20000/3MB、`ota_1` 0x320000/3MB（app 约 1MB，余量充足）。
- 启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`：
  - OTA 写入完成后 `esp_ota_set_boot_partition()` 把新分区标记为待验证；
  - 新固件首次启动处于 `ESP_OTA_IMG_PENDING_VERIFY`，**必须**在健康后调用
    `esp_ota_mark_app_valid_cancel_rollback()`；
  - 若在 mark-valid 之前崩溃/重启，bootloader 自动回滚到旧版本；
  - 直接 `idf.py flash` 烧录的镜像不受影响（不走 OTA 状态机）。

### 3.2 健康判定（mark-valid 时机）

`cpe_ota_mark_running_valid()` 在启动后 `CPE_OTA_VALIDATE_DELAY_MS`（默认 60s）执行一次：

- 选择"AP/Web 可用即算健康"，而不是"4G 上线"：4G 依赖外部模组/运营商，不能作为固件健康前提；
- 这样即使模组没插，新固件也不会被回滚；而启动即崩溃/复位循环会早于 60s，触发回滚。

### 3.3 Web API（管理面，仅 SoftAP）

| 方法 | 路径 | 鉴权 | 说明 |
| --- | --- | --- | --- |
| GET | `/api/ota/status` | 会话 | 返回运行版本、运行分区、下一启动分区、`pending_verify`、回滚是否启用、是否正在上传 |
| POST | `/api/ota/upload` | 会话 + CSRF，需先改密 | body 为完整 app 镜像（`application/octet-stream`），按 `Content-Length` 流式写入；成功响应后自动重启 |
| POST | `/api/system/reboot` | 会话 + CSRF | 重启设备（运维用） |

上传限制与失败处理：

- 必须带 `Content-Length`，且 `0 < len <= 目标分区大小`；
- 超时（`recv_wait_timeout`）内收不满 → 中止 OTA，旧分区保持不变；
- `esp_ota_end()` 校验失败（不是合法镜像）→ 400 + 保持旧版本；
- 上传过程中断电：新分区写坏，bootloader 仍从旧分区启动（otadata 未切换）；
- OTA 进行中再次上传 → 409，避免并发写同一分区。

### 3.4 Web 页面（下一步）

- 新增"系统升级"页：显示当前版本/分区、选择 `.bin` 上传、进度条、确认对话框；
- 上传期间禁用其它操作，避免升级中改配置；
- 升级完成提示"设备将重启，请等待约 1 分钟并重新连接 AP"。

### 3.5 后续（可选）

- 通过 `esp_https_ota` 从 URL 升级（需要 4G WAN 出网 + 证书/哈希校验）；
- 固件签名校验（`esp_ota_write_with_offset` + 安全启动链）；
- 双分区回滚统计/上报（回滚次数写入 NVS，状态页显示）。

## 4. 验收清单

| # | 测试 | 期望 |
| --- | --- | --- |
| 4.1 | 上传合法固件 | 进度正常，自动重启，版本号变化 |
| 4.2 | 新固件启动后 60s | `OTA image validated` 日志；重启后仍是新版本（不回滚） |
| 4.3 | 上传过程中拔电/断网 | 重启后仍运行旧版本 |
| 4.4 | 上传非固件文件（如 txt/图片） | 400，旧版本不变 |
| 4.5 | 上传超过分区大小的文件 | 413/400，旧版本不变 |
| 4.6 | 上传中途第二次上传 | 409 |
| 4.7 | 新版启动后人为制造 boot loop（如断点测试） | bootloader 回滚到旧版本 |
| 4.8 | 未登录/无 CSRF 调用 OTA 接口 | 401/403 |

### 4.x 首轮 curl 验收记录（2026-10-18，固件 `6f4dac7`，设备 `ESP-CPE-F2BD`）

| 项 | 结果 |
| --- | --- |
| 4.8 未登录 `GET /api/ota/status` | ✅ 401 |
| 4.8 无 CSRF `POST /api/system/reboot` | ✅ 403 `csrf_invalid` |
| 4.8 无 CSRF `POST /api/ota/upload` | ✅ 403（服务端不读 body 直接关闭；大 body 时客户端可能看到连接重置） |
| 4.1 上传合法固件（`build/usb_ecm_4g_module.bin`） | ✅ `200 {"status":"ok","message":"ota_ready","reboot":true}`，耗时 2.27s，设备重启 |
| 4.4 上传非固件文件 | ⚠️ 返回 400，但错误码是 `upload_interrupted`；根因是 `esp_ota_write()` 在首个字节不是 `0xE9` 时返回 `ESP_ERR_OTA_VALIDATE_FAILED`。已改进：先读首块校验 magic 再动 flash，并映射为 `400 invalid_firmware_image`（待重新构建验证） |
| 4.1/4.2 重启后状态确认 | ⏳ 设备 OTA 重启后 PC 的 Wi-Fi 自动切回了原来的网络（192.168.1.x），未能继续用 curl 确认 `ota_1` 与 60s 后的 mark-valid；需要重新连接 `ESP-CPE-F2BD` 后补测 |
| 4.5/4.6/4.3 | ⏳ 待补 |

注意：回滚保护需要**带 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 的 bootloader**。当前设备上的 bootloader 是开启该选项之前烧录的，
所以本轮 OTA 虽然能切换分区，但 `PENDING_VERIFY`/回滚状态机要到用 `idf.py -p COM6 flash` 整片重烧一次（含 bootloader）后才真正生效；
之后再用 OTA 升级即具备回滚能力。

## 5. 风险与约束

1. **模组挂死**：OTA 重启后 4G 可能挂死（见 2.w），升级后需要人工插拔模组——这是当前硬件限制，
   不阻塞 OTA 功能本身，但量产前必须解决；
2. **回滚启用后的行为变化**：`idf.py flash` 不受影响，但 OTA 镜像若因健康判定过严（60s 内）未 mark-valid
   会被回滚；`CPE_OTA_VALIDATE_DELAY_MS` 需按现场启动时间调；
3. **SoftAP 上传带宽**：3MB 镜像在 802.11n/40MHz 下约 10~30s，需确认 httpd 超时与浏览器行为；
4. **HTTPS 自签证书**：浏览器告警影响体验，需在 P3-2 权衡"自签 + 提示"或"仅配网窗口"方案。

## 6. 变更记录

| 日期 | 内容 | 提交 |
| --- | --- | --- |
| 2026-10-18 | 建立 Phase 3 计划，确定 P3-1~P3-4 里程碑与 OTA 设计 | 待提交 |
| 2026-10-18 | P3-1 后端：`cpe_ota`（流式写入/回滚保护/健康后 mark-valid）、`/api/ota/status`、`/api/ota/upload`、`/api/system/reboot`；启用 `BOOTLOADER_APP_ROLLBACK_ENABLE` | 待提交 |
| 2026-10-18 | 首轮 curl 验收：鉴权 401/403、合法固件上传并重启 PASS；非法文件错误码修正（先校验镜像 magic 再写 flash）；回滚需先整片重烧含新 bootloader | 待提交 |
