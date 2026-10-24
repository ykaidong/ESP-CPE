# Agent 工作规则（ESP-CPE 仓库）

本文件记录用户明确要求 AI 代理遵守的持久化规则。每次会话开始或恢复后，先读本文件。

## 1. ESP-IDF 编译验证规则（用户 2026-10-17 要求，2026-10-21 更新）

1. **默认由用户手动完成编译验证**（用户 2026-10-21 指示：“除非指定，编译验证均由我来完成”）。
   AI 代理不得自行开始构建；只有用户明确指定“本次由 AI 构建”时才可执行。
2. 若由 **AI 代理构建**：不能直接执行 `idf.py build` —— 必须先启动 ESP-IDF 的 PowerShell 环境。
   用户提供的启动命令为：

   ```powershell
   powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' }"
   ```

   该命令来自 Windows Terminal 中 ESP-IDF 的终端配置文件（`C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` 是 EIM 安装的激活脚本）。
   在此环境就绪后再执行 `idf.py build`（或等价 cmake/ninja 流程）。
3. 若由 **用户手动构建**：AI 代理只提供命令与预期结果，等待用户回传 `build_log.txt` 错误段后再修改代码。
4. 构建产物：`build/`、`sdkconfig`、`build_log.txt` 均为本地生成物，已被 `.gitignore` 忽略（`build_log.txt` 视需要清理）。
5. 本环境实测（2026-10-17）：DSH 沙箱内 `idf.py` 因 asyncio 命名管道被拒（WinError 5），代理构建需要对该条构建命令一次性授予 `danger-full-access`；首次构建通过，产物 `build/usb_ecm_4g_module.bin`。

## 2. host 单元测试（用户 2026-10-24 起）

1. 纯逻辑改动（`components/web_console/router_web/json_util.c`、`station_names.c`、
   `components/cpe_core/cpe_config.c` 的校验部分等）**由 AI 代理自行跑 host 单测**，无需用户授权：
   它不用 `idf.py`、不用真机，只是一段 gcc 编译的普通进程。

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\tests\host\run.ps1
   ```
2. `tests/host/` 不被任何组件 `CMakeLists.txt` 引用，不影响固件构建。它用 `tests/host/stubs/` 下的
   桩头文件替代 ESP-IDF，只编译被测的纯逻辑；**桩里不允许长出真实行为**（NVS/PSA/锁一律空实现），
   否则测试测的是桩。新增用例时沿用这个边界：只测不依赖硬件、RTOS 调度和持久化的逻辑。
3. cJSON 取自 `managed_components/`（首次 `idf.py build` 后才有），是真实源码，不是桩。

## 3. 其他既有约定

- 设备/模组：ESP32-S3 + 16MB Flash + 通用 USB ECM 模组。AT 用于可选遥测（`components/cpe_at/`），并在主动重启前以 `AT+CFUN=1,1` 尽力复位模组；ECM 数据链路不依赖 AT。
- 阶段推进：每阶段 `idf.py build` 通过（默认用户手动，AI 仅在明确指定时执行）后再进入下一阶段；当前进度见 `docs/IMPLEMENTATION_PLAN.md`。
- 不提交 `sdkconfig`/`sdkconfig.old`；关键配置写入 `sdkconfig.defaults*`。
- 本地 fork：`components/iot_usbh_cdc`（3.1.5）、`components/iot_usbh_ecm`（0.4.4），修改需更新 CHANGELOG 与版本。
