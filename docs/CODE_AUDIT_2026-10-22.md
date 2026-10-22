# 代码审计报告（2026-10-22）

审计范围：`main/`、`components/` 下自研组件（app_wifi、cpe_core、cpe_wan、cpe_at、cpe_maintenance、web_console）。
未纳入：`components/iot_usbh_cdc`、`components/iot_usbh_ecm`（本地 fork 的第三方驱动，单独维护）。
审计维度：架构合理性、实现一致性、代码冗余、潜在风险。
本次审计为只读分析，未修改任何代码、未编译。

对应整改计划见 `docs/REFACTOR_PLAN.md`。

---

## 一、架构与分层

### A1. 缺一个"系统服务层"：跨组件动作散落复制（最严重）

"重启""恢复出厂""踢站点"这类跨组件动作没有归宿，同一语义被复制多份：

- 重启前宽限 + 重启：`web_console/router_web/api_system.c:93-94`(500 ms)、`api_system.c:117-118`(500 ms)、`api_ota.c:290-291`(800 ms)、`main/reset_button.c:140-141`(300 ms)、`cpe_maintenance/cpe_maintenance.c:409-410`(200 ms)——5 处、4 个魔法延时。
- 恢复出厂编排：`api_system.c:55-96`（config + 昵称 NVS + 维护配置 + 登出全部会话 + 重启）与 `main/reset_button.c:122-141` 两份，新增清理项必漏一侧。
- 踢站点：`api_station.c:90-98` 直接调 `esp_wifi_ap_get_sta_aid()` / `esp_wifi_deauth_sta()`，绕过 app_wifi。

建议：新增 `system_restart(reason)`、`cpe_system_factory_reset()`、`app_wifi_sta_disconnect(mac)` 三个服务接口，各调用方只调接口。

### A2. 配置归属倒置

`main/Kconfig.projbuild:41-378` 定义了属于下层组件的选项：`CPE_USB_TASK_*`(41-57)、`CPE_LINK_TIMEOUT_MS`(59)、`CPE_IP_*`(68-88)、`CPE_MAX_RECOVERY_ATTEMPTS`(90)、`CPE_MAINT_*`(114-184)、`CPE_AT_*`(349-377)；这些组件自身没有 `Kconfig.projbuild`，可配置性被上层 main 拥有。
反向依赖：`cpe_core/cpe_config.c:248` 使用 `app_wifi/Kconfig.projbuild:3` 定义的 `CONFIG_SERVER_IP`。

建议：各组配置迁回所属组件；SERVER_IP 改由 app_wifi 提供运行时 getter。

### A3. cpe_wan 越界承担全系统重启，并与 app_wifi 互相依赖

`cpe_wan/wan_net.c:21-22`（include app_wifi）、`:82,132,138`（拿 AP netif、调 `app_wifi_deauth_all_stations()`）、`link_manager.c:292`（`cpe_wan_reboot()` 内 deauth + `esp_restart()`）。
调用方遍布：`api_system.c:94,118`、`api_ota.c:291`、`main/reset_button.c:141`、`cpe_maintenance.c:410`。

建议：重启/清理下沉到 cpe_core（或独立 system 组件），cpe_wan 只保留 WAN 职责。

### A4. 状态真相多份 + 启动顺序靠注释

- 链路状态同时存在于 `cpe_core/cpe_status`、`cpe_wan/link_manager.c:79-93` 的 `lm_ctx`、`cpe_wan/usb_wan.c:46` 的 `s_ecm`；写入点分散在 `wan_net.c:46-53`、`usb_wan.c:174-188`、`link_manager.c:143-165`。
- `main/main.c:63-189` 的顺序约束全靠注释，无检查；已产生空窗：`status_led_start()`(69) 早于 `cpe_core_init()`(98)，LED 任务读到全零状态。

建议：链路状态唯一来源放 cpe_status，`lm_ctx` 只保留恢复预算；启动依赖用显式 sequencer 或各模块自愈。

### A5. 存储层未收敛

4 套 NVS 命名空间并存：`cpe_config`（cpe_core）、`cpe_maint`（cpe_maintenance）、`sta_names`（`web_console/router_web/station_names.c:112,131,147` 自行 `nvs_open`）、以及 web 侧其它用途。

建议：统一 `cpe_kv` 小封装或统一走 cpe_config 命名空间。

### A6. 路由表分散、鉴权无集中拦截

`modem_http_config.c:102-124` 串联 6 个 `api_*.c` 的注册函数；18 个 URI 与 `max_uri_handlers=20`(`:131`)、`CPE_WEB_ROUTE_LOG_MAX=24`(`http_common.c:37`) 三处无编译期关联；"哪个 URI 需要会话/CSRF"只能逐个文件看。

建议：集中一张 `{uri, method, handler, auth}` 静态表 + 静态断言。

### A7. 模块内反向依赖

`cpe_core/cpe_config.c:18,445`：配置层 include 并调用 `cpe_boot_count_clear()`（恢复层）。

建议：由 `cpe_core.c` 或复位编排层统一调用。

---

## 二、实现混乱与一致性

### B1. 并发原语混用四种

`cpe_config.c:136` 递归互斥 / `cpe_auth.c:85` 互斥 / `cpe_status.c:30` 互斥 / `cpe_ota.c:32,88` portMUX 临界区；同类状态（上传中、会话）分别用互斥锁和临界区保护。锁顺序（auth→config）未在头文件写明。

### B2. 日志级别滥用

正常流程用 `ESP_LOGW`：`api_ota.c:289`（OTA 成功落盘）、`api_system.c:85,116`（控制台重启）、`cpe_maintenance.c:370,404,443`（定时维护）、`cpe_ota.c:206,209,234,254,268`、`main/reset_button.c:112,123,133,138`（按键按下）。W 级刷屏会淹没真正异常。

### B3. 三个文件共用 `TAG = "cpe"`

`usb_wan.c:33`、`wan_net.c:28`、`link_manager.c:30`。现场日志无法区分驱动 / 状态机 / 网络服务。建议改为 `cpe_wan` / `cpe_net` / `cpe_link`。

### B4. 错误处理风格不一致

- `main/main.c:82,92,109,137` 四处 `while (1) { vTaskDelay(10000); }` 永久挂死，而同文件 146/152/159 行同类失败只 WARN 继续。
- `main.c:63,115,125`、`cpe_config.c:445` 用 `(void)` 吞掉返回值，紧邻的调用却检查并记日志。
- `usb_wan.c:138` `wan_ecm_teardown()` 返回 void，`:171-178` uninstall 失败静默 return，调用方按成功继续推进恢复预算。

### B5. 错误响应与错误码不统一

`close_socket` 参数：`http_request.c:98-105` 一律 true，各 `api_*.c` 的 400 分支一律 false，`api_ota.c` 一律 true。
400 错误码三套命名：`api_station.c:137` `invalid_request`、`api_system.c:167` `invalid_request_body`、`api_wlan.c:204` `invalid_ssid`。

### B6. GET 鉴权样板重复 6 处，POST 有模板而 GET 没有

`api_status.c:17-24`、`api_ota.c:51-58`、`api_system.c:124-131`、`api_station.c:23-30`、`api_wlan.c:110-117,329-336`：均为 `reject_body + require_session + return ESP_FAIL`。POST 已有 `cpe_web_post_begin()` 统一前置，GET 缺对称入口，新增 GET 极易漏鉴权。

### B7. 魔法数字

helper task 栈/优先级两处各写一遍（`api_wlan.c:55-56`、`modem_http_config.c:237`）；`link_manager.c:127,135,221,303,322,385,425`（5000/2000/1000/500）；`usb_wan.c:123,159,316`（1000/20/300）；重启宽限 800/500/300/200 ms。同类时序参数中有一部分进了 Kconfig，调参者看不到全貌。

### B8. 文档与实现不符（4 处）

- `cpe_wan.h:31-33` 说返回 last error，`wan_net.c:197-218` 实际返回 **first** error。
- `cpe_wan.h:76` 说请求在链路管理未运行时被丢弃，`link_manager.c:436-439` 实际置位常驻 `volatile bool`（sticky）。
- `cpe_wan.h:8-11,49-51` 宣称 bounded escalation，但 IP/枚举两条路径无上界（见 D7）。
- `usb_wan.c:53-63` 枚举过滤器无论 VID/PID 是否匹配都 `return true`，`CONFIG_CPE_ECM_VID/PID` 名为 filter 实为配置值选择器。

---

## 三、代码冗余

| 编号 | 冗余点 | 证据 |
|---|---|---|
| C1 | 同一套配置校验抄 3 份，其中两份漏 admin_user | `cpe_config.c:102-124` / `cpe_config_store.c:368-374` / `app_wifi.c:79-83` |
| C2 | 15 个 setter 是"取锁-赋值-放锁"模板复制约 200 行，且无 deinit | `cpe_status.c:59-250` |
| C3 | OTA 状态复位临界区块 4 处，其中两处仅错误码不同 | `cpe_ota.c:176-186,192-197,213-217,235-240` |
| C4 | 同组件两套随机串生成器（字母表/截断规则不同） | `cpe_auth.c:45-55` vs `cpe_config_store.c:99-113` |
| C5 | boot count 两函数重写了本文件已有的 `write_u32()` | `cpe_recovery.c:92-126` vs `:44-62` |
| C6 | NVS `open→get/set→commit→close` 四段式样板 5 处 | `cpe_maintenance.c:100-156,205-232`、`station_names.c:106-153` |
| C7 | "拆栈 + 清零计数 + 延时 + 回 BOOT 相"三份 | `link_manager.c:212-223,348-357,365-387` |
| C8 | JSON 转义逐字节 `appendf("%c")`，1 KB 响应上千次格式化 | `json_util.c:74-77,113-114` |
| C9 | 死字段/死接口：`ecm_runs`/`reboot_runs`（只 ++ 不读）、`cpe_ota_running_pending_verify()` 仅内部调用却公开且带日志副作用、`cpe_web_station_registry_deinit` 无调用方 | `cpe_maintenance.c:61-62,403`、`cpe_ota.h:79`/`cpe_ota.c:243-258` |
| C10 | MAC/IP 缓冲长度 `18/13/16/33` 硬编码在结构体与函数签名两边 | `http_internal.h:79-81` vs `station_names.c:32,39` |

---

## 四、潜在风险

### D1（高）httpd 单任务被慢操作独占

esp_http_server 只有一条任务处理全部 socket。OTA 上传在 handler 内同步收包写 flash（空闲超时 180 s，`api_ota.c:27,114-293`）、重启前 `vTaskDelay(500~800 ms)`（`api_system.c:93,117`、`api_ota.c:290`）、`/wlan_advance` 内联执行含 flash 写（`api_wlan.c:406-423`）。一次上传即可让控制台分钟级无响应（单认证用户可 DoS）。
`api_wlan.c:300` 已有 helper task 模式可复用。

### D2（高）静态资源 `"/*"` GET 完全免鉴权

`static_files.c:84-120` 的 `rest_common_get_handler` 只做 body 检查与 `uri_is_safe()`（仅防路径穿越），不校验会话；注册于 `:162-167`。SPIFFS 内任何文件未登录可读。登录页必须公开，故应为白名单而非全开。

### D3（高）事件回调里做重活

`WIFI_EVENT_AP_START` 回调 `wan_net.c:174-189` → `wan_link_online()` → NAPT + `esp_netif_get/set_dns_info`（阻塞 IPC 到 tcpip 线程）+ `esp_wifi_ap_get_sta_list` + deauth（含 100 ms 延时）。长时间占用事件循环任务，与 Wi-Fi/IP 事件互相阻塞。

### D4（高）非 tcpip 线程直接调用 netif action

`usb_wan.c:152` `esp_netif_action_stop()`、`:280` `esp_netif_action_connected()` 由链路管理任务直接调用（`link_manager.c:121,131`），与 TCP/IP 线程并发改 netif 状态。应改走 `esp_netif_tcpip_exec()` 或公开同步 API。

### D5（高）`s_ecm` 无锁 + ECM install 失败泄漏

`usb_wan.c:46` 的 `s_ecm` 被链路管理任务改写（`:138-189`，含 `vTaskDelay(20)`），同时被事件回调读（`wan_net.c:41,58`），可能拿到悬垂句柄（代码注释已承认无锁）。
`usb_wan.c:217-224` install 失败只置 `driver = NULL`（注释自认无法销毁），而 `link_manager.c:127,135` 每 5 s 无限重试 → 每次泄漏一个驱动对象。

### D6（中）持锁执行慢操作

`cpe_config.c:393-441`：`cpe_config_factory_reset()` 在配置递归锁内调用 `cpe_config_store_pbkdf2()`（1 万次迭代），与 `cpe_config_priv.h:38-41` 自己写的"持锁期间不得调用"矛盾。
`cpe_auth.c:112-183`：`cpe_auth_login()` 在 auth 锁内做密码校验，一次登录（或密码爆破）阻塞所有并发请求的会话校验。

### D7（中）"有界恢复"名不副实 + 默认 netif 不回退

`link_manager.c:236-246`：`ip_attempts` 仅用于显示，从不与上限比较；DHCP 重试用尽后每 90 s 无限重建 ECM。`:275`：`enum_reset_attempts` 每轮清零，无模组时每 60 s 无限断电 root port（若此行为是有意的——保持 AP/Web 可用——应修正 `cpe_wan.h:49-51` 的表述）。
`usb_wan.c:268` 每次 bring_up 设默认 netif，`:138-189` teardown 从不回退。

### D8（中）cpe_at 句柄与信号量

端口句柄在 generation 变化时直接丢弃不 close（`cpe_at.c:972-975,1007-1017,1078-1086`）；复位信号量存在陈旧 give，超时后污染到下次调用（`:1141-1144` 与 `:1130`）；事件注册失败每 1 s 无限重试且不上报状态（`:1019-1038`）。

### D9（低）缓冲区零余量与边界假设

`wan_net.c:61-64` `char ip[16]` 装 "255.255.255.255" 无余量且未检查 `snprintf` 返回；`cpe_at.c:84-86` `s_imei[16]`、`s_model/s_fw[32]` 同理；`:674-677` `+CME ERROR` 用硬编码下标解析；`cpe_config_store.c:99-113` `random_password(buf, len)` 只收长度不收容量（内部写 `buf[len]='\0'`，调用点用魔法数 12）。

### D10（低）其它

- `reset_button.c:135-141`：`while (button_is_pressed())` 无超时，按键卡死则工厂复位永远无法完成。
- `cpe_ota.c:96-134`：busy 检查与置位分处两个临界区（TOCTOU），幸有 `esp_ota_begin` 的 `ESP_ERR_OTA_ALREADY_IN_PROGRESS` 兜底（`:123`），实际风险低。
- `station_registry.c:76-79`：链表锁超时仅 100 ms，超时即丢弃该站点节点。
- `app_wifi.c:56-58,299-304,348-356`：启动状态由 3 个散落 bool 表达，无 `stop()`/`esp_wifi_deinit()` 出口。
