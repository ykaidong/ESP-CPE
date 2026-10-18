# ESP-CPE 产品化实施计划（基于官方示例重新开始）

| 项目 | 内容 |
| --- | --- |
| 基线 | 官方 `usb_ecm_4g_module` 示例（当前仓库，`main/main.c` 495 行，无本地 fork） |
| 目标 | ESP32-S3 + 16MB Flash + 通用 USB ECM 4G 模组 → 可量产的 4G CPE 产品基座 |
| 依据 | `C:\Users\fytech\Desktop\ESP-CPE\AUDIT_REPORT.md`（114 条，P0=6 / P1=25 / P2=50 / P3=33）与 `UPSTREAM_ASSESSMENT.md` |
| 硬件口径 | ESP32-S3、16MB Flash、无 AT 依赖、USB ECM 数据通道；硬件差异全部 Kconfig 化 |
| 节奏 | 分阶段推进，每阶段 `idf.py build` 通过后汇报，再进入下一阶段 |

---

## 一、总体原则

1. **不再做"整栈拆装 + 无界重启"**：审计证明该方案会把"网络没起来"放大成"设备无限重启"。恢复分三层：
   - L1 软恢复：只重建 ECM 驱动/端口（USB Host 与 CDC 驱动保持）；
   - L2 枚举恢复：根端口 VBUS 掉电重连（可选，Kconfig）；
   - L3 受控重启：NVS 启动计数**先写后重启**，超过预算进入"故障但可管理"模式，不再重启。
2. **AP/Web 先行**：SoftAP + Web 后台在任何 4G/USB 状态下都可用；4G 链路由后台任务管理，失败时后台显示故障页并允许重试。彻底消除"无 SIM/无 IP = 整机变砖"。
3. **安全默认值**：AP 默认 WPA2 + 每台唯一随机口令；管理员口令首启随机且与 AP 口令同值（用户必须已能连上 Wi-Fi 才可能登录），首次登录强制修改；不再有编译期固定凭据。
4. **认证升级**：HTTP Basic → 会话 Cookie（HttpOnly + SameSite=Strict）+ CSRF Token；所有状态变更接口校验 Content-Type/CSRF；删除通配 CORS。
5. **配置不落坏值**：所有 Wi-Fi 参数服务端强校验；先应用后持久化，失败即回滚；非法配置返回 400，不写 NVS；启动时校验 NVS，坏值回退默认值。杜绝 `channel:"999"` 类配置砖。
6. **根修优先于重试**：NEW_DEV 事件丢失（审计 P1-11）用"client 注册后枚举快照补发"根修；CDK/ECM 生命周期缺陷在本地 fork 中逐条修（见阶段二）。
7. **工程卫生**：不提交 `sdkconfig`/`sdkconfig.old`；关键非默认项写入 `sdkconfig.defaults`；fork 组件升版本并记录 CHANGELOG；README 重写为本产品文档。

---

## 二、阶段划分与审计条目映射

### 阶段一：阻断项 + 产品骨架（本次实施）

| # | 工作项 | 审计依据 | 验收标准 |
| --- | --- | --- | --- |
| 1.1 | 新建 `components/cpe_core`：配置存储/校验、随机凭据、管理员口令哈希、会话认证、状态机、故障恢复计数、恢复出厂 | P0-3、P1-13、P1-15、8.1-#6/#7 | 首启生成唯一 AP 口令（串口打印）；NVS 坏值回退；登录/会话/CSRF 单元可测 |
| 1.2 | 重写 `main/main.c`：AP/Web 先行 + 后台链路管理器 + 三层恢复 + 幂等 USB Host 安装 | P0-2、P1-11、P1-19、8.1-#2/#3/#4 | 拔掉 4G/无 SIM 时 AP+Web 仍可用；无 `ESP_ERROR_CHECK(usb_host_install)`；无无界重启路径 |
| 1.3 | 枚举过滤器：默认关闭，改为 Kconfig（VID/PID + 配置号） | P1-9、8.1-#5 | 默认构建不含"强制配置 2"；开启仅对指定设备生效 |
| 1.4 | 分区表改 16MB + 双 OTA + storage；`sdkconfig.defaults` 同步；清理 `sdkconfig` 跟踪 | P2（OTA/构建卫生）、8.3-#1 | 16MB 分区表构建通过；`sdkconfig` 不再入库 |
| 1.5 | fork `iot_usbh_cdc` / `iot_usbh_ecm` 到 `components/`，修 P0 级生命周期缺陷 | P0-1、P1-2、P1-3、P1-4 | deinit 关端口、close 排空在途 URB、open 错误路径不 double free |
| 1.6 | 驱动修 TX 路径三连缺陷 + ECM 假 link-up + NEW_DEV 补发 | P1-7、P1-8、P1-11 | 提交失败归还令牌；out_mux 不泄漏；MAC 解析失败返回错误；NEW_DEV 不丢 |
| 1.7 | Web 认证与默认安全值改造（由 `components/web_wifi` 工作流完成） | P0-3、P1-12、P1-13、P1-14、P1-15、P1-16、P1-17 | 无固定凭据；401 `Connection: close`；超大 Content-Length 提前 413；参数校验；XSS/CSRF 修复 |
| 1.8 | 构建验证 + 文档（本文件、README 更新、凭据/恢复策略说明） | 8.3-#5 | `idf.py build` 0 error；生成 bin 可烧录 |

> 阶段一已删除示例诊断组件与配置残留：`components/network_test`（PING/公网 IP/测速）、
> `sdkconfig.ci.at`、`sdkconfig.ci.p4fullspeed`、`sdkconfig.defaults.esp32s2/.esp32p4`；
> 阶段二如需诊断能力（公网 IP/测速/AT），按审计条目重新接入并修缺陷。

### 阶段二：驱动与 Web 全面加固（下一阶段）

> 详细任务、优先级、验收与里程碑见 **[PHASE2_PLAN.md](PHASE2_PLAN.md)**。
> 按用户要求：先把现有功能做成熟（驱动 + Web），AT 诊断作为新功能在 Phase 2 完成后单独立项。

- CDC：`port_close()` 两阶段延迟释放、回调链表加锁、控制传输长度校验、IN 轮询耗尽状态上报、通知重提交；`driver_install` 失败置 NULL。
- ECM：`itf+1`/IAD 解析改为按 Union/EC 功能描述符定位；通知长度校验；`wMaxSegmentSize` 上限钳制；deinit 幂等。
- `espressif__usb`：评估升级/补丁（枚举 FSM SECOND_RESET 卡死、无传输超时、事件队列满）。
- Web：请求体读取公共函数（长度上限/NUL/分片）、JSON 转义、站点链表加锁、登录限速与锁定、`json_parse_end()`、网络 IO 去 `ESP_ERROR_CHECK`、CSP/nosniff。
- iperf/`network_test`/`modem_at`：阶段一已删除示例实现；阶段二按需重新接入，并先修对应缺陷（P1-18、R-04/R-05、R-15 等）。

### 阶段三：产品化（再下一阶段）

- OTA：双分区 + 回滚开关 + 升级 API/页面 + 版本号语义；`esp_https_ota` 或本地上传。
- 安全：Secure Boot / Flash 加密 / NVS 加密；口令 PBKDF2 迭代提升；会话过期与登出；首次配网窗口策略。
- 可靠性：长稳测试（拔插 ×100、无 SIM ×N、慢注册、Web 压测）、看门狗、日志分级与脱敏、故障码。
- 出厂：恢复出厂（GPIO 长按 + Web + 串口命令）、设备标签/二维码流程、批量测试工具。

---

## 三、阶段一架构

```text
app_main
 ├─ nvs_flash_init / esp_netif_init / event_loop
 ├─ cpe_core_init
 │    ├─ cpe_config_init   （NVS 校验、首启随机凭据、串口打印）
 │    ├─ cpe_auth_init     （PBKDF2 口令、会话表、CSRF）
 │    └─ cpe_status_init   （线程安全状态快照）
 ├─ app_wifi_start()       （SoftAP：来自 cpe_config，校验失败回退默认值）
 ├─ modem_http_start()     （SPIFFS + httpd：登录/状态/配置/故障页）
 └─ cpe_link task          （后台链路管理器，永不阻塞主流程）
      ├─ usb_host_install（幂等、显式错误处理）
      ├─ usbh_cdc_driver_install / iot_eth_new_usb_ecm / iot_eth_install
      ├─ 事件：IOT_ETH CONNECTED/DISCONNECTED、IP_EVENT_ETH_GOT_IP
      ├─ ONLINE：set_default_netif + ip_napt_enable + 上游 DNS 下发给 AP
      └─ 失败：无设备 → 模组复位/根端口掉电；无 IP → 只重启 DHCP；ECM 卡死 → 有界重启；FAULT 保底（AP/Web 保持）
```

> 启动各阶段的日志、状态机迁移、每个等待点的超时与原因，见 **[RUNTIME_FLOW.md](RUNTIME_FLOW.md)**。

### 3.1 共享接口契约（跨组件冻结）

- `cpe_config.h`：`cpe_config_init()`, `cpe_config_get()`, `cpe_config_set_ap()`, `cpe_config_set_sta()`, `cpe_config_set_device_name()`, `cpe_config_factory_reset()`, 校验函数。
- `cpe_auth.h`：`cpe_auth_login()`, `cpe_auth_session_valid()`, `cpe_auth_csrf_valid()`, `cpe_auth_logout()`, `cpe_auth_change_password()`, `cpe_auth_must_change()`。
- `cpe_status.h`：`cpe_status_get()`, `cpe_status_set_state()/set_usb()/set_link()/set_ip()/set_error()`。
- `web_wifi`（由 Web 工作流负责实现）：
  - `esp_err_t app_wifi_start(void);`
  - `esp_err_t app_wifi_apply(const cpe_config_t *cfg);`（应用 AP 参数，失败返回错误且不落盘）
  - `esp_netif_t *app_wifi_get_ap_netif(void);`
  - `esp_err_t modem_http_start(void);` / `esp_err_t modem_http_stop(void);`
- `main` 对外不暴露重试/拆栈 API；恢复逻辑全部在后台任务内。

### 3.2 凭据策略（阶段一开始执行）

- 出厂/首启：AP SSID = `ESP-CPE-XXXX`（MAC 后 4 位十六进制）；WPA2 口令 12 位随机（去除易混字符），仅存 NVS。
- 管理员：用户名 `admin`，初始口令 = AP 口令，`must_change=true`；首次登录必须改密（8~63 位）。
- 串口在首启/恢复出厂时打印一次：`AP SSID / AP 密码 / 管理员初始密码 / 访问地址 http://192.168.4.1`。
- 恢复出厂（Web 鉴权+CSRF 触发，阶段三加 GPIO 长按）：**保留设备唯一的 AP SSID/口令**（避免 Web 复位后用户无法再连上 AP），其余配置回默认；管理员口令重置为 AP 口令并强制下次登录修改；清除启动计数后重启。
- 日志/接口不回显口令；`GET /wlan_general` 只返回 `has_password`。

### 3.3 恢复策略（阶段一开始执行）

> 完整运行时流程、状态机与全部等待点见 **[RUNTIME_FLOW.md](RUNTIME_FLOW.md)**。以下为策略摘要。

| 场景 | 判定 | 动作 | 失败后果 |
| --- | --- | --- | --- |
| 无 USB 设备 | 30s 无 link，且总线上没有已枚举设备 | 依次尝试：模组 RESET GPIO → 根端口 VBUS 掉电（Kconfig 开启时）→ 重启 USB Host 栈；最多 `CPE_USB_ENUM_RESET_ATTEMPTS`（3）次 | FAULT，退避 60s 后重试；**不重启整机** |
| link up 无 IP | 链路已 up，90s 内 DHCP 未拿到 IP | **保持 ECM/USB 会话**，只重启 DHCP 客户端，再给 90s 窗口，最多 `CPE_IP_DHCP_RESTARTS`（2）次；仍未拿到才拆 ECM 重建 | FAULT，后台继续重试；**不重启整机** |
| ECM 链路卡死 | 模组已枚举，但 ECM 链路在 `CPE_MAX_RECOVERY_ATTEMPTS`（3）轮内始终起不来 | 读取 NVS 启动计数，`boot_cnt < CPE_MAX_REBOOTS`（3）时 `esp_restart()` | 计数耗尽后进入 FAULT（AP/Web 可用，后台每 60s 重试） |
| 健康 | link up + got_ip 连续 120s | 清除 NVS 启动计数并同步 Web 状态 | — |

> 关键产品行为：**没插模组/无 SIM 永远不会导致整机重启**（重启救不了这两类故障）；只有"设备已枚举但 ECM 接口卡死"才允许有界重启。所有阈值见 `menuconfig → CPE Application Configuration`。

---

## 四、阶段一验收清单

- [ ] `idf.py build` 通过（ESP-IDF v6.1，target=esp32s3，16MB 分区）。
- [ ] 默认构建：AP 为 WPA2、口令随机唯一；固件/前端无 `esp32/12345678` 类固定凭据。
- [ ] 无 4G 模组/无 SIM：设备仍启动 AP + Web；故障页显示状态；后台持续重试；不出现重启循环。
- [ ] 配置接口：非法 channel/口令/SSID 返回 400 且不落 NVS；Wi-Fi 应用失败回滚。
- [ ] 认证：Basic 已移除；登录返回会话 Cookie + CSRF；无会话访问 401 且 `Connection: close`；改密强制生效。
- [ ] USB：`usbh_ecm_deinit()` 关闭端口；`port_close()` 不释放仍在途的 URB；TX 提交失败不丢令牌。
- [ ] 枚举过滤器默认关闭；开启时只对配置的 VID/PID 生效。
- [ ] `sdkconfig` 不再被 git 跟踪；`sdkconfig.defaults` 与 16MB/OTA 分区一致。

---

## 五、风险与待确认

1. **实机验证缺口**：本计划与审计均为静态分析，阶段一只能保证编译通过；拔插/无 SIM/慢注册/Web 攻击链需上板验证（阶段三测试清单）。
2. **模组差异**：`wMaxSegmentSize`、接口号/配置布局、`iMACAddress` 格式差异较大，阶段二改为按 Union/EC 功能描述符解析；建议提供模组 `lsusb -v` 描述符转储。
3. **NEW_DEV 丢失**仍需实机日志确认（审计待确认项 #1）：阶段一先实现快照补发，后续用日志统计验证。
4. **管理员初始口令 = AP 口令**是为了解决"没有串口时如何首次登录"；若产品要求更高，可在阶段三改为设备标签独立随机码。
5. **HTTPS**：阶段一仍为 HTTP（仅 AP 网段 + 会话认证）；阶段三再引入 HTTPS/证书或"仅配网窗口"。

---

## 六、Phase 1 实施记录（2026-10-17）

### 6.1 已交付

| 交付物 | 位置 | 说明 |
| --- | --- | --- |
| 产品核心组件 | `components/cpe_core/` | 配置存储/校验、随机凭据、PBKDF2 口令、会话+CSRF、状态机、NVS 启动计数、恢复出厂 |
| 新启动架构 | `main/main.c` | AP/Web 先行；USB Host 只装一次；后台链路管理器（L1 ECM 重建 / L2 端口掉电 / L3 有界重启 / FAULT 保底） |
| 分区与构建 | `partitions_16m_ota.csv`、`sdkconfig.defaults*`、`.gitignore` | 16MB 双 OTA；`sdkconfig` 不再入库；关键补丁配置进 defaults |
| 驱动本地 fork | `components/iot_usbh_cdc/`（3.1.3）、`components/iot_usbh_ecm/`（0.4.2） | P0-1/P1-2~P1-8/P1-11 逐条修复；NEW_DEV 补发；DEFER_DATA_POLL；enum_filter_cb/fsls_only 透传；枚举速度日志 |
| Web 管理面 | `components/web_wifi/` | 会话+CSRF 认证、参数校验、DoS/XSS/CSRF/口令回显修复、状态/故障页、恢复出厂 |
| 文档/脚本 | `README.md`、`docs/PHASE1_TEST_PLAN.md`、`scripts/build.ps1` | 首次登录流程、恢复策略、上板验收清单、构建辅助 |

### 6.2 验证状态

- **ESP-IDF v6.1 构建：通过（2026-10-17，AI 代理执行，ESP32-S3 / 16MB / 双 OTA）**
  - `build/usb_ecm_4g_module.bin` = **1024624 B (0xFA270)**，最小 app 分区 3MB，剩余 67%；
  - bootloader 0x5820（39% free）、`storage.bin` 1MB、`ota_data_initial.bin`、16MB 分区表均正常生成；
  - `main`、`components/cpe_core`、`components/web_wifi`、两个 USB fork 编译 **0 warning**；
  - 构建方式：按 AGENTS.md 规则先启动 ESP-IDF PowerShell 环境后执行 `idf.py build`（详见 `AGENTS.md`）。
- 静态检查已执行：无固定凭据残留、Web 网络 IO 无 `ESP_ERROR_CHECK`、main 无 USB 整栈拆装、fork 版本/CHANGELOG 已更新、Web 与驱动负责人分别做了 cppcheck / `-fsyntax-only` 自检；两份独立对抗式审查（Web / USB 驱动）发现的问题已修复或记录。
- 实机验证项见 `docs/PHASE1_TEST_PLAN.md`（首次登录、无 SIM 故障模式、拔插、配置校验、DoS/XSS 回归）。Phase 1 剩余工作仅为上板验收，通过后进入 Phase 2。

### 6.3 阶段一遗留（不计入阻断项）

- 端口句柄 TOCTOU（审计 C2）未做引用计数，仅收窄窗口；
- 上游控制传输无 HCD 超时（H-1），极端设备可能拖住 ECM 恢复；
- Web "重启设备"独立接口未新增（仅有恢复出厂）；HTTPS/Secure Cookie 留待阶段三；
- AT/light-sleep/iperf/network_test 示例代码已从仓库删除（阶段二如需诊断能力再按需接入）。

### 6.4 复核期补充（构建等待期间）

- 恢复策略细化为"无枚举设备不重启、已枚举但 ECM 卡死才受控重启"，避免现场没插模组时整机反复重启；
- ECM 重建时同步清空 link/IP 状态，消除状态残留导致的误判；
- 初始化失败（NVS/config/netif/task）改为打印错误 + 空闲降级，不再 `ESP_ERROR_CHECK` abort（避免远程设备重启循环）；
- 恢复出厂改为保留 AP 凭据（Web 触发不会把用户锁在 AP 外）；
- 已用 IDF `gen_esp32part.py` 校验 16MB 分区表、`node --check` 校验前端 JS、静态脚本核对内部符号声明/定义一致（82 个符号，0 个未声明）。

### 6.5 Web 复核修复记录（独立审查代理，无 P0）

- 修复：AP 重配导致连接重置时 XHR 误报 `http_0` → 前端不再把"已生效"显示为"操作失败"；
- 修复：客户端列表 4s 轮询会擦除正在编辑的主机名 → 编辑期间暂停刷新、仅在客户端页签激活时轮询；
- 修复：`json_get_string_unescaped()` 区分"字段缺失"与"值非法/超长"，超长口令/设备名、`if_hide_ssid:2`、`max_sta:1.5` 现在返回 400 而非静默忽略；
- 修复：设备名持久化失败时不再留下已生效的 AP 变更（先持久化设备名，AP 失败时回滚）；
- 修复：Web 组件改为始终编译（删除"关闭即编译失败"的 `EXAMPLE_ENABLE_WEB_ROUTER` 开关）；`cpe_core/esp_netif` 提为公共依赖；
- 修复：恢复出厂同时清空 NVS 启动计数与 `sta_names` 站点昵称；前端文案改为"保留 Wi-Fi 名称与口令"；
- 修复：`init_fs()` 仅在 `esp_spiffs_mounted()` 为真时把 INVALID_STATE 当"已挂载"；`max_open_sockets` 7；
- 修复：运行期改信道改用 `esp_wifi_set_channel()`（带宽仍用 `esp_wifi_set_bandwidth()`，失败回滚）；
- 修复：sessionStorage 不可用时 CSRF 通过 `window.name` 跨页面保留，避免隐私模式登录循环；
- 已知留待阶段二：`rssi/public_ip/modem_model` 暂无数据源（需 AT/诊断接入）；`modem_http_stop()` 预留给 OTA/重启流程。

### 6.6 上板实测发现并修复的问题（Phase 1 验收期）

| # | 现象 | 根因 | 修复 |
| --- | --- | --- | --- |
| 1 | ECM 模组（VID 2CA3/PID 4006）枚举成功、接口/端点找到，但 `Failed to parse the MAC address descriptor (len 26)` → 3 次后 link down | fork 的 `_parse_mac_descriptor()` 把"字节数"与"hex 字符数"混淆：每 2 个字符才累加 1（最大 6），却用 `== 12` 判断成功 → 永远失败 | 修正为 6 字节判断；MAC 读取/解析失败不再中止 ECM 配置，改用本地管理 MAC 回退（fork 0.4.2） |
| 2 | ESP32 运行中复位（4G 模组独立供电、不断电）→ `USBH: Dev 0 EP 0 Error` / `CHECK_SHORT_DEV_DESC FAILED`，之后无法恢复 | 模组 USB 栈未随 ESP32 复位而复位；ESP-IDF 枚举失败后不会自行重试 | 新增 `CPE_MODULE_RESET_GPIO`（启动时 + 枚举恢复时脉冲模组 RESET，需硬件接线）；无 RESET 线时按 `CPE_USB_ENUM_RESET_ATTEMPTS` 先尝试 VBUS 掉电（若启用），最后**重启 USB Host 栈**（卸载 host 撤掉总线端接让模组感知断开，再重新安装触发一次全新枚举）；二者都不可用时进入 FAULT（AP/Web 可用） |
| 3 | Web 后台：状态页"客户端数量/已连接数/剩余连接数"一直显示 `--`；登录页图标显示为方框（iconfont 字体未随 SPIFFS 打包）；登录框偏左、页面空旷；新标签页/刷新后"退出登录"无反应 | ① 4s 轮询只在客户端页签激活时启动，初始化时不再拉取列表；② `style.css` 引用 `fonts/iconfont.*` 但 SPIFFS 无该字体；③ 旧 CSS 把登录卡绝对定位在 left:15%/top:20%；④ CSRF token 只存 sessionStorage（按标签页），新标签页取不到 → POST 403 被静默吞掉 | ① 页面加载时拉取一次客户端数量（不启动隐藏轮询）；② 登录页改用已有图片、去掉 iconfont；③ cpe.css 覆盖登录/状态页布局（居中卡片、白底分区卡片、移动端适配）；④ 服务端下发非 HttpOnly 的 `CPE_CSRF` Cookie（SameSite=Strict，双提交），登出不再强制 CSRF 且失败时前端提示；启动计数清零后同步刷新状态 |
| 4 | 状态页排版不美观：基本状态三个点位被推到页面两端、网络详情"标签在左、值贴右边缘" | 旧布局用 `flex:1` 的占位线段把三项撑满整行；`.status-row{justify-content:space-between}` 使值总是贴右 | 基本状态改为三张等宽状态卡（图标 + 标签 + 值，间距 12px）；设备状态改为三块指标卡；网络详情改为两列键值网格，值左对齐紧跟标签；≤720px 自动单列 |
| 5 | 复位后 ECM 枚举/配置正常、`4G link up`，但反复出现 `Link up but no IP after 20000 ms`，每轮都拆 ECM 重建 | ① 默认 IP 超时只有 20s，而模组在 USB 复位后重新附着网络（PDP/DHCP 服务器就绪）可能需要数十秒；② 超时立即 `ecm_teardown()` 会重置模组 USB 接口，下一轮又只给 20s，形成"重建—超时—再重建"循环，反而让 DHCP 永远等不到可用窗口 | `CPE_IP_TIMEOUT_MS` 默认改为 90s；新增 `CPE_IP_DHCP_RESTARTS`（默认 2）：链路保持、仅重启 DHCP 客户端并打印其状态，仍无 IP 才重建 ECM；链路断开/拿到 IP 时重置计数 |
| 6 | 仓库残留官方示例的 PING/诊断组件与其它芯片配置：`components/network_test`（每 5s ping 8.8.8.8、公网 IP、上传/下载测速）、`sdkconfig.ci.at`、`sdkconfig.ci.p4fullspeed`、`sdkconfig.defaults.esp32s2/.esp32p4`，以及 `EXAMPLE_WEB_MOUNT_POINT`、未实现的 `CPE_FACTORY_RESET_GPIO` | 示例改造为产品时未清理：`network_test` 已不被 `main` 引用（map 中无 ping 符号）但仍参与编译；示例 CI 引用的 `CONFIG_EXAMPLE_*` 在任何 Kconfig 中都已不存在；其它芯片的 defaults 与 ESP32-S3 产品无关 | 删除 `components/network_test`、两个 `sdkconfig.ci.*`、`sdkconfig.defaults.esp32s2/.esp32p4`；`EXAMPLE_WEB_MOUNT_POINT` → `CPE_WEB_MOUNT_POINT`；删除未实现的 `CPE_FACTORY_RESET_GPIO`；文档同步 |
| 7 | 恢复出厂后重启没有打印凭据，串口监视器仍会错过早期输出；用户误以为旧密码失效或系统异常 | 恢复出厂/重启时打印的时机太早，且明文管理员口令无法从哈希恢复；GPIO0 在复位期间被按住会让芯片进入下载模式，不能做"启动时按住" | 新增 `main/reset_button.c`：复位键（默认 GPIO0，低有效）**启动后 10 秒内双击** → 打印当前 AP 口令；管理员口令若仍等于 AP 口令则一并打印，已修改则标注"明文不可恢复"（避免误报）。**开机后长按 10s** → 恢复出厂并重启（等待松开再重启，避免 strapping）。恢复出厂不再依赖"重启后打印一次"的 NVS 标记 |

> 现场结论：对外部供电的 4G 模组，产品硬件必须提供 RESET_N→GPIO 或受控供电，否则主机复位后可能枚举失败。README 已补充接线说明。

