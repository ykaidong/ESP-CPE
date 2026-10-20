# 代码审计报告（2026-10-19）

审计范围：应用层代码、本地 fork 的 USB 驱动、Web 前端与静态资源。
审计维度：代码冗余、重复、架构合理性、实现缺陷。
本次审计为只读分析，未修改任何代码。

## 修复状态（2026-10-19）

- **D1–D10 已修复**（D8 同时修复了 B5）。
- **B4、B6、B7 已修复**：
  - B4：新增 `cpe_ct.c`，合并 `cpe_config.c` 与 `cpe_auth.c` 的常量时间比较。
  - B6：`upgrade.js` 改用 `/api/ota/status` 返回的 `max_image_size`，删除硬编码的 3MB。
  - B7：删除 `style.css` 中指向不存在的 `images/`、`fonts/` 的变量与相关死规则，消除控制台 404。
- 验证阶段发现并修复了三个不在原始列表中的运行时问题：
  - **ECM RX 双释放崩溃**：v0.4.3 的 RX 缓冲复用违反了 `iot_eth` 的缓冲区所有权约定，链路 up 后首次 DHCP 交换即触发 `tlsf_free` 断言。v0.4.4 已回退为每包分配，详见 D8。
  - **重启后 Windows 保留陈旧 Wi-Fi 关联**：受控重启前没有主动 deauth，异常复位后 AP 又以同一 BSSID 立即重建，导致终端显示已连接但流量被丢弃。已新增 `app_wifi_deauth_all_stations()`（受控重启路径）和异常复位后的随机 BSSID；SSID、密码、IP 均不变。
  - **未登录时的 401 探测**：首页与登录页在浏览器没有 CSRF token 时不再请求 `/api/status`，未登录访问/刷新不再产生 401。
- **B2、B3 已修复**：
  - B2：`byId`、`bindMenu` 与双 tab 切换逻辑收进 `CPE`（`api.js`），各页面只保留业务逻辑。
  - B3：新增 `layout.js` 统一生成 header 与左侧导航，四个 HTML 页面只保留占位节点。
- **A5 已处理（删除 STA 半成品）**：移除 `cpe_config_t` 的 `sta_ssid/sta_password`、相关 NVS 键与 `cpe_config_set_sta()`，以及 `app_wifi.c` 的 `build_sta_config()`、APSTA 分支和 STA 事件处理。产品上行仍是 USB ECM；将来需要 Wi-Fi 上行时再完整引入配置 API、状态与 UI。
- **B1 已修复**：新增 `cpe_web_post_ctx_t` 与 `cpe_web_post_begin()` / `cpe_web_post_end()`（`http_common.c`），统一前置校验、会话/CSRF、读体、深度检查、解析与缓冲擦除；三个 API 文件中的 9 个 JSON POST handler 全部迁移。二进制 OTA 上传与 GET handler 不在模板范围内。
- **待处理**：第三节死代码。其中 `rssi/public_ip/modem_model` 属阶段二预留，`usb_ecm_get_cdc_port_handle` 为上游驱动 API，`modem_http_stop` 为 OTA/重启预留。

---

## 一、架构问题

### A1. `modem_http_config.c` 是 2590 行的“上帝文件”（最严重）

`components/web_wifi/router_web/modem_http_config.c` 一个文件里混了 9 类职责：

- HTTP 底层工具（`send_error_response`、`reject_unexpected_body`、`request_origin_is_same_origin`）
- 手写 JSON 构造器 `sbuf_*`（约 100 行）
- JSON 解析封装（`json_depth_ok`、`jparse_ctx_t` 使用模板）
- 请求体读取与错误映射（`read_json_body`/`send_body_error`）
- 会话与 CSRF 校验、Cookie 头构造
- 业务 handler：登录、改密、状态、WLAN、客户端列表/改名/删除、恢复出厂、重启、OTA 上传/查询
- 客户端昵称的 NVS 读写
- Wi-Fi 事件桥接
- 服务器生命周期

后果：任意改动都要在近 2600 行里跳转，handler 之间共享静态缓冲（`s_session_cookie_hdr` 等），无法独立测试。建议按职责拆为 `http_util.c` / `json_util.c` / `api_auth.c` / `api_wlan.c` / `api_station.c` / `api_system.c` / `api_ota.c`，至少先把 handler 与工具层分开。

### A2. `web_wifi` 组件职责混淆

同一组件同时负责 SoftAP（`app_wifi.c`）与 Web 管理面（`router_web/*`）。两者生命周期、恢复策略、测试方式完全不同。建议拆成 `app_wifi` 与 `web_console` 两个组件，`main.c` 分别编排。

### A3. 配置持久化模型是“全量重写”

`cpe_config.c:232 persist_locked()` 每次 setter 都把全部 NVS key 重写一遍（SSID、口令、信道、带宽、max_sta、STA、管理员 hash……）。改一个字段也触发几十次 `nvs_set_*`，失败时无法做单事务回滚，只能靠调用方手动“先应用、失败再写回”。建议：

- 提供单一的 `cpe_config_update(apply_fn)` 事务接口，或
- 只写变更的 key，并集中处理失败回滚。

### A4. 链路管理状态机是单函数 200 行

`main/main.c` 的 `link_manager_task()` 把在线判定、ECM 重建、根端口掉电、模组复位、NVS 计数、受控重启全部塞在一个函数里，状态迁移靠局部变量和 sleep 表达。建议抽成显式状态表（状态/事件/动作/超时），每种恢复动作独立函数，便于单测与日志追踪。

### A5. STA 功能半成品（已处理：删除）

`app_wifi.c` 有完整的 `build_sta_config()` 路径，`cpe_config_set_sta()` 也存在，但无任何配置入口、无 UI、无状态上报。要么删掉，要么补齐；现在属于“看起来支持、实际不可用”的架构负债。

**处理结果**：已删除全部 STA 半成品代码。`cpe_config_set_sta()` 无调用方且默认 STA SSID 为空，STA 实际永远不会启用；产品上行仍是 USB ECM。若将来需要 Wi-Fi 上行，应完整引入配置 API、运行时应用、状态上报与 UI，而不是恢复这段未接线的代码。

---

## 二、重复代码

### B1. 每个 POST handler 的模板照抄 7~9 次

登录、改密、WLAN 通用、WLAN 高级、客户端改名、客户端删除、重启、恢复出厂等 handler 都重复：

```c
esp_err_t pre = precheck_post(req, true);
char session[SESSION_BUF_LEN] = {0};
esp_err_t err = require_session_mutable(req, session);
char body[MAX_JSON_BODY + 1];
body_result_t body_result = read_json_body(...);
if (!json_depth_ok(body, body_len, JSON_MAX_DEPTH)) { ... }
jparse_ctx_t jctx; json_parse_start(...); ...
```

建议抽成 `api_post_begin(req, &ctx)` / `api_post_end(&ctx)` 或一个统一的分发层，把“前置校验 + 读体 + 深度检查 + 解析 + 清理”收进一处。

**已修复（2026-10-20）**：9 个 JSON POST handler 已统一到 `cpe_web_post_begin()` / `cpe_web_post_end()`，并通过选项区分登录（无会话）、登出/恢复出厂（可选 CSRF）、常规变更（session + CSRF + 强制改密检查）以及 body 必需/可选。

### B2. 前端四个 JS 文件各写一份 `byId`

`api.js`、`index.js`、`wlan.js`、`login.js`、`upgrade.js` 均重复 `byId`；`index.js` 与 `wlan.js` 还各自重复 `bindMenu()` 与 `switchTab()`。建议统一挂到 `CPE` 命名空间下（例如 `CPE.byId`、`CPE.bindMenu`）。

### B3. HTML 页面复制公共头部/菜单

`index.html`、`wlan.html`、`upgrade.html`、`login.html` 各自维护一份 header/menu 结构，任一导航改动要改四处。纯静态页面没有模板系统，可退而求其次：用一段 `layout.js` 注入菜单，或至少保持同一结构由脚本生成。

### B4. 常量时间比较实现两份

- `cpe_config.c:69 ct_equal()`（字节数组）
- `cpe_auth.c:56 token_equal()`（字符串）

逻辑相同，建议合并到一个 `cpe_crypto`/`cpe_util` 模块，避免将来只修一处。

### B5. `iot_usbh_ecm.c` 重复设置事件位

```c
// iot_usbh_ecm.c:655
xEventGroupSetBits(ecm->event_group, ECM_DEV_CONNECTED);
// iot_usbh_ecm.c:657
xEventGroupSetBits(ecm->event_group, ECM_DEV_CONNECTED);
```

连续两行完全相同，应删一行（事件位本身重复置位无害，但属于明显笔误，会误导读者认为两处有语义差别）。

### B6. OTA 容量常量在前后端各写一份

`upgrade.js:11` 硬编码 `MAX_OTA_SIZE = 3 * 1024 * 1024`，而服务端 `/api/ota/status` 已返回 `max_image_size`（`upgrade.js:55` 也读取了但没用于校验）。分区表一旦调整，前端校验就与后端不一致。应改为使用 `info.max_image_size`。

### B7. CSS 资源重复与缺失

四个页面都同时加载 `style.css`（1355 行，上游 demo 遗留）与 `cpe.css`。`style.css` 引用但仓库中不存在的资源：

- `css/style.css:23` `../images/bg.png`
- `css/style.css:39-42` `../fonts/iconfont.woff2/.woff/.ttf`

每次打开控制台都会产生 404。建议删除未使用的 demo 样式，只保留 `cpe.css`。

---

## 三、死代码 / 未接线接口

| 符号 | 位置 | 状态 |
| --- | --- | --- |
| `cpe_status_set_public_ip()` | `cpe_status.c:93` | 从未调用，但 `/api/status` 仍序列化 `public_ip/has_public_ip`（`modem_http_config.c:1362-1364`） |
| `cpe_status_set_rssi()` | `cpe_status.c:109` | 从未调用，`/api/status` 仍输出 `rssi/rssi_valid`（1365-1366） |
| `modem_model` / `modem_fw` | `/api/status` 1367-1369 | 无任何 setter，永远为空串 |
| `modem_http_stop()` | `modem_http_config.c:2553` | 声明并实现，从未调用 |
| `usb_ecm_get_cdc_port_handle()` | `iot_usbh_ecm.c:961` | 无调用方 |

这些字段/接口会让前端和后续维护者误以为功能存在。建议：要么接通（公网 IP/RSSI 需要 modem AT 或诊断通道），要么连同 API 字段一起删除；`modem_http_stop` 可保留但应在注释中明确“未接线”或直接移除。

---

## 四、实现缺陷

### D1. OTA 首块超时路径未排空请求体（中）

`modem_http_config.c:1577-1581`：首块 `recv` 超时后直接返回 408，没有调用 `ota_drain_unread_body()`。而同文件所有其他错误分支（1553、1559、1593、1600 等）都做了排空。浏览器此时仍在发送 body，服务端关连接会让 XHR 报 `network_error`，用户看到“网络中断”而不是真实的“上传超时”。应与其他分支一致，先排空剩余 body 再响应。

### D2. OTA 超时按“上传起点”计时，而非空闲时间（中）

`modem_http_config.c:1572/1578/1634`：`start_us` 在读取第一块之前设置，`OTA_UPLOAD_TIMEOUT_US` 为 180 秒总时长。只要某次 `recv` 超时，且此时距上传开始已超过 180 秒，就立即中止——即使客户端一直在持续发送、只是速度较慢。3MB 固件在弱 Wi-Fi 下完全可能超过 180 秒。建议改为“最后一次成功收到数据的时间”作为基准，表示空闲超时。

### D3. OTA 错误码语义被压平（中）

- `modem_http_config.c:1654-1656`：流式写入中除 `INVALID_IMAGE` 外，所有错误（包括真实的 flash 写失败）都映射为 `400 upload_interrupted`，客户端无法区分“文件不对/网络断了/设备存储坏了”。
- `modem_http_config.c:1660-1662`：`cpe_ota_end()` 的任何错误（含 `esp_ota_set_boot_partition` 失败这类系统错误）统一返回 `invalid_firmware_image`。

建议保留错误来源：镜像校验失败 → `invalid_firmware_image`；flash/分区错误 → `500 ota_write_failed` 或类似；客户端断开 → `400 upload_interrupted`。

### D4. `/api/ota/status` 的 `status` 字段值不一致（低）

`modem_http_config.c:1490` 返回 `"status":"200"`，而其他接口（如 OTA 上传 1667、恢复出厂 1437）返回 `"status":"ok"`。前端目前不依赖该字段，但契约不一致，应统一为 `"ok"`。

### D5. `online_time_s` 名实不符（中）

- 服务端 `modem_http_config.c:977` 存的是 `esp_timer_get_time()`（微秒时间戳），`:2068` 原样输出为 `online_time_s`。
- 前端 `index.js:83` 又用 `(nowTime - online_time_s) / 1000000` 计算在线时长。

字段名说“秒”，实际是“微秒级绝对时间戳”，还依赖客户端与服务端计时基准一致。建议服务端直接计算并输出 `online_time_s = (now - start_time) / 1e6`，前端只做展示。

> 附带问题：`index.js:83` 中 `nowTime` 若来自 `Date.now()`，与设备 `esp_timer_get_time()`（开机计时）根本不在同一时间基准，计算出的在线时长会是巨大值或负数。请务必核对。

### D6. 前端用魔数判断故障状态（低）

`index.js:245`：`if (status.state === 4)`。`4` 对应枚举的哪个状态完全不可读，一旦枚举调整就静默出错。应由服务端直接下发字符串状态（如 `"fault"`），或前端定义具名常量并集中映射。

### D7. CDC 驱动安装失败路径不回滚 USB Host（中）

`iot_usbh_cdc.c:1582-1606`：`err:` 分支释放了 CDC 对象和信号量，但在本函数内已成功 `usb_host_install()` 的情况下，既不调用 `usb_host_uninstall()`，也不停止仍在 `usb_host_lib_handle_events()` 里阻塞的 `usb_lib_task`。若上层重试安装：

- 新的 `usb_lib_task` 走 `usb_host_lib_info() == ESP_OK` 的“已被他人安装”分支（`:216-218`）并继续；
- 于是出现两个 task 同时调用 `usb_host_lib_handle_events(portMAX_DELAY)`，违反 USB Host 库“事件处理应单线程”的约束，属于潜在并发缺陷。

建议安装失败时统一拆除本函数创建的全部资源（含 host 库与 lib task）。

### D8. ECM 驱动健壮性问题（中/低）

- `iot_usbh_ecm.c:655/657` 重复置位（见 B5）。
- `_ecm_config_attempt()` 在锁外读取 `ecm->config_attempts`，属于无同步的计数器读取；统计用途影响不大，但应统一在锁内读取。
- `usbh_ecm_init()` 对 `ecm->mediator` 解引用前无 NULL 检查（发送 `IOT_ETH_STAGE_LL_INIT` 时），构造顺序变化会直接崩溃。
- `_usbh_ecm_recv_data_cb` 每个 RX 包 `malloc` 一次。**不要改为复用缓冲区**：`mediator->stack_input*()` 最终调用 `esp_netif_receive()`，后者接管缓冲区所有权，并在 pbuf 释放时通过 `driver_free_rx_buffer`（`eth_l2_free() -> free()`）释放它。若复用同一指针，第 2 个包会触发 `tlsf_free` 的 “block already marked as free” 断言（已在 v0.4.3 上实测复现，v0.4.4 回退）。若要去除堆抖动，只能改为“协议栈释放后归还的对象池”，不能使用单缓冲。

### D9. 密码生成存在模偏差（低）

`cpe_config.c:91`：`esp_random() % (sizeof(PASSWORD_ALPHABET) - 1)`。字母表长度通常不是 2 的幂，取模会产生极小偏差。安全影响可忽略，但修复成本极低（拒绝采样）。同时 `apply_defaults_locked()`（`:221-225`）在 PBKDF2 失败时仅把 hash 清零并继续启动，结果是设备带着一个“永远无法通过认证”的管理员 hash 运行；应视为致命错误并阻止启动或重新生成。

### D10. 静态状态变量的并发假设未加锁（记录性）

- `cpe_ota.c:31-34` 的 `s_upload_in_progress/s_update_handle/...` 在注释中声明“单 HTTP 任务所以安全”，但 `cpe_ota_get_info()` 会从状态接口读取，若将来增加 worker 或启用多任务 HTTP 配置即成竞态。
- `cpe_recovery.c` 的启动计数是 NVS 读-改-写，无互斥。

目前可接受，但建议在头文件中把“必须单线程调用”写成契约，或直接加互斥量。

---

## 五、做得较好的部分

- 安全基线扎实：会话 Cookie（HttpOnly/SameSite）+ CSRF 双提交、登录锁定指数退避、PBKDF2、恒定时间比较、前端 `textContent` 渲染且无 `innerHTML`。
- USB 驱动 fork 的修补有清晰 CHANGELOG，延迟释放、回调快照、枚举重放等处理考虑周全。
- OTA 流程有 `PENDING_VERIFY` 回滚保护、先校验镜像头再擦写分区、错误路径排空 body 的细节设计（除 D1 遗漏一处）。
- 恢复策略“AP/Web 先行、链路后台恢复、有界重启”符合产品化要求。

---

## 六、建议优先级（2026-10-19 更新）

1. ~~先修：D1、D2、D3、D5（OTA 与状态接口的实际行为缺陷），D7（驱动重试竞态）。~~
   已完成，D1–D10 全部修复（含 B5）。
2. ~~B6（OTA 容量前后端一致）、B7（静态资源 404）、B4（常量时间比较去重）。~~
   已完成并验证。
3. ~~B2/B3（前端公共函数与布局去重），需要浏览器逐页验证。~~
   已完成。
4. **下一步**：B1（先拆文件，再抽 handler 模板），与 A1 合并处理，避免在单一巨型文件中改两遍。
5. **随后**：第三节剩余死代码（已分类的预留项保持不动）。

B5 已随 D8 修复；B4/B6/B7 与 B2/B3 已完成；A5 已按删除处理；B1 与其余死代码待处理。
