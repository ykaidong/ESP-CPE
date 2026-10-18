# Phase 2 验收与测试计划（驱动 + Web）

> 目标：验证 Phase 2 的驱动/枚举加固与 Web 安全改动在实机上成立。
> 前置：已完成 Phase 1 上板验收；模组 RESET 线暂不接线（软件兜底路径按"无 RESET 线"验收）。
>
> 辅助脚本：
> - `scripts/serial_log_check.ps1`：串口日志统计与异常检测
> - `scripts/web_security_test.py`：管理面安全回归（HTTP，Python 标准库）

## 0. 构建与烧录

```powershell
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd C:\Users\fytech\Desktop\usb_ecm_4g_module
idf.py build
idf.py -p COM6 flash monitor
```

期望：app 镜像仍能放入 3MB OTA 分区；自有代码 0 warning；启动日志无 panic。

## 1. Web 安全（M2.1）

| # | 测试 | 期望 | 结果 |
| --- | --- | --- | --- |
| 1.1 | 启动日志 | 出现 `Web server will listen on SoftAP interface "ap0" only`；httpd 绑定该接口 | ☐ |
| 1.2 | 连 AP 访问 `http://192.168.4.1` | 正常打开登录页 | ☐ |
| 1.3 | 从 4G 网段（模组分配的 192.168.225.x）访问设备 80 端口 | 无法连接/无响应（管理面只在 AP 侧） | ☐ |
| 1.4 | 未登录 GET `/api/status` | 401 + `Connection: close` | ☐ |
| 1.5 | 未登录 POST 带 `Content-Length: 2000000000` | 立即 413/401，httpd 不卡死，其它请求仍可响应 | ☐ |
| 1.6 | 登录后 POST 不带 CSRF | 403 | ☐ |
| 1.7 | 登录后 POST 畸形 JSON / 超长字段 | 400，NVS 不变 | ☐ |
| 1.8 | 非法 Wi-Fi 参数（channel=999、口令 >63、`if_hide_ssid:2`、`max_sta:1.5`） | 400，AP 不重启，重启后配置仍合法 | ☐ |
| 1.9 | 连续 5 次错误登录 | 第 6 次返回 429，锁定 60s；再触发一次锁定变 120s | ☐ |
| 1.10 | 登录后空闲 30 分钟再操作 | 会话失效，跳回登录页 | ☐ |
| 1.11 | 客户端名称 XSS 载荷 `"><img src=x onerror=alert(1)>` | 页面纯文本显示，无脚本执行 | ☐ |
| 1.12 | 恢复出厂后 | AP 凭据保留；管理员口令 = AP 口令；启动计数清零 | ☐ |

自动化：`scripts/web_security_test.py` 覆盖 1.4~1.9（锁定测试会真的锁 60s，默认需要 `--include-lockout` 才执行）。

### 1.x 自动化执行记录（2026-10-18）

- 设备地址：用户提供 192.168.1.4 不可达；实际设备 SoftAP 为 **192.168.4.1**（PC 在 192.168.4.2），测试按 192.168.4.1 执行。
- 默认凭据 `admin` / `FooTdEh2F4jv` 登录成功（`must_change=true`，说明尚未完成首次改密）。
- 结果：未登录 401 + `Connection: close`、超长 Content-Length 被快速断开且服务器仍存活、登录/会话/CSRF、无 CSRF 403、畸形 JSON 400、弱口令 400 —— **全部 PASS**。
- 因 `must_change=true`，`/wlan_advance` 等状态变更接口被强制改密拦截（403 `password_change_required`），相应校验项（1.8）待改密后补测。

## 2. 驱动与枚举（M2.2/M2.3）

| # | 测试 | 期望 | 结果 |
| --- | --- | --- | --- |
| 2.1 | 正常启动接模组 | 枚举成功、`ECM driver ready`、`4G WAN IP`、`CPE online` | ☐ |
| 2.2 | 反复热插拔模组 20 次 | 每次恢复 link/IP；无 panic；无重复端口打开错误 | ☐ |
| 2.3 | 无 RESET 线，运行中按 ESP32 复位 10 次 | 每次要么恢复枚举，要么稳定停在 FAULT；**不重启整机** | ☐ |
| 2.4 | 无 SIM 持续 10 分钟 | `link up but no IP` → FAULT，AP/Web 可用，无重启 | ☐ |
| 2.5 | 不接模组上电 10 分钟 | no-device FAULT，AP/Web 可用，无重启 | ☐ |
| 2.6 | 正常运行时观察 10 分钟日志 | **不出现** `Replaying the CONNECTED event for N existing device(s)` 周期刷屏；无 `CHECK_SHORT_DEV_DESC` 后卡死 | ☐ |
| 2.7 | 在线稳定 2 分钟 | `Healthy for 120 s, boot counter cleared`（仅首次有计数时打印） | ☐ |
| 2.8 | 连续 20 次插拔后的堆水位 | 无明显持续下降（日志/`heap` 诊断） | ☐ |

串口日志检查：

```powershell
# 先保存 monitor 输出到 build_log.txt，再运行
.\scripts\serial_log_check.ps1 -LogFile .\build_log.txt
```

## 3. 复位键（Phase 1.5 回归）

| # | 测试 | 期望 | 结果 |
| --- | --- | --- | --- |
| 3.1 | 启动后 10s 内双击 GPIO0 | 打印当前 AP 口令；管理员口令未改则一并打印 | ☐ |
| 3.2 | 改管理员口令后双击 | AP 口令打印；管理员行显示"changed, plaintext not stored" | ☐ |
| 3.3 | 长按 10s | 恢复出厂；松开后重启 | ☐ |
| 3.4 | GPIO0 在复位期间被按住 | 不用于打印（会进下载模式）；文档已说明 | ☐ |

## 4. 已知接受风险（不阻塞 Phase 2）

| 风险 | 说明 |
| --- | --- |
| 端口句柄 TOCTOU | 未引入引用计数，仅两阶段释放收窄窗口；需实机长稳观察 |
| HCD 控制传输无超时 | 上游 `usb` 组件限制；极端设备可能长时间占用，需上游升级 |
| `iot_eth_install()` 失败泄漏 | API 限制；失败概率低，已有 60s 退避 |
| HCD assert/abort | 上游 managed 组件未 fork |

## 5. 结果记录

| 日期 | 固件版本 | 执行人 | 结果 | 备注 |
| --- | --- | --- | --- | --- |
|  |  |  |  |  |
