/*
 * System status / online client list page for the ESP-CPE management console.
 *
 * All values coming from the device (station host names, IP addresses, error
 * strings, ...) are rendered with textContent or value assignment only - never
 * with innerHTML - so a stored host name cannot inject markup (audit P1-16).
 */
'use strict';

(function () {
    var STATION_URL = '/system/station_state';
    var CHANGE_NAME_URL = '/system/station_state/change_name';
    var DELETE_DEVICE_URL = '/system/station_state/delete_device';
    /* Keep in sync with cpe_status.h (CPE_STATE_FAULT). */
    var STATE_FAULT = 4;

    var stationList = [];
    var stationTimer = null;
    var statusTimer = null;
    /* Uptime is reported once per status poll; count locally from the last
     * synced value instead of advancing a counter on every timer tick. */
    var uptimeBaseS = 0;
    var uptimeBaseMs = 0;
    var uptimeSynced = false;
    var uptimeTimer = null;
    /* Set when a status poll fails: the page can no longer reach the device, so
     * the local estimate would keep ticking and every other value on screen
     * would silently stay at its last reading. */
    var statusStale = false;
    var lastStatusAt = null;
    /* The API value is read before the response travels back, so a poll usually
     * lags the local estimate slightly. Only correct clearly conflicting
     * values; re-basing every poll makes the seconds repeat. */
    var UPTIME_RESYNC_THRESHOLD_S = 3;
    /* While a host name is being edited the periodic refresh must not rebuild
     * the table, otherwise the input is wiped every 4 s. */
    var stationEditing = false;

    function setText(id, text) {
        var node = CPE.byId(id);
        if (node) {
            node.textContent = text;
        }
    }

    function pad2(value) {
        return value < 10 ? '0' + value : String(value);
    }

    function formatDuration(totalSeconds) {
        var seconds = Math.max(0, Math.floor(totalSeconds));
        var hours = Math.floor(seconds / 3600);
        var minutes = Math.floor((seconds % 3600) / 60);
        var rest = seconds % 60;
        if (hours > 0) {
            return hours + '小时' + minutes + '分钟' + rest + '秒';
        }
        if (minutes > 0) {
            return minutes + '分钟' + rest + '秒';
        }
        return rest + '秒';
    }

    function formatClock(date) {
        return pad2(date.getHours()) + ':' + pad2(date.getMinutes()) + ':' +
            pad2(date.getSeconds());
    }

    function makeButton(label, className, index) {
        var button = document.createElement('input');
        button.type = 'button';
        button.value = label;
        button.className = className;
        button.setAttribute('data-index', String(index));
        return button;
    }

    function makeCell(text) {
        var cell = document.createElement('td');
        cell.textContent = text;
        return cell;
    }

    function renderStationRow(station, index) {
        var row = document.createElement('tr');

        var nameCell = document.createElement('td');
        var nameInput = document.createElement('input');
        nameInput.type = 'text';
        nameInput.className = 'list-name';
        nameInput.disabled = true;
        nameInput.maxLength = 32;
        /* value assignment (not innerHTML) keeps host names inert. */
        nameInput.value = typeof station.name_str === 'string' ? station.name_str : '';
        nameCell.appendChild(nameInput);
        row.appendChild(nameCell);

        row.appendChild(makeCell(typeof station.mac_str === 'string' ? station.mac_str : ''));
        row.appendChild(makeCell(typeof station.ip_str === 'string' ? station.ip_str : ''));

        var onlineSeconds = Number(station.online_time_s) || 0;
        row.appendChild(makeCell(formatDuration(onlineSeconds)));

        var actionCell = document.createElement('td');
        var actionWrap = document.createElement('div');
        actionWrap.className = 'flex flex-jcc';

        var editButton = makeButton('编辑', 'edit-btn', index);
        var deleteButton = makeButton('删除', 'del-btn', index);

        editButton.addEventListener('click', function () {
            if (editButton.value === '编辑') {
                stationEditing = true;
                if (stationTimer) {
                    clearTimeout(stationTimer);
                    stationTimer = null;
                }
                nameInput.disabled = false;
                nameInput.focus();
                editButton.value = '确认';
            } else {
                renameStation(index, nameInput.value, nameInput, editButton);
            }
        });

        deleteButton.addEventListener('click', function () {
            if (window.confirm('确定要断开并删除该客户端吗？')) {
                deleteStation(index);
            }
        });

        actionWrap.appendChild(editButton);
        var divider = document.createElement('div');
        divider.className = 'edit-line';
        actionWrap.appendChild(divider);
        actionWrap.appendChild(deleteButton);
        actionCell.appendChild(actionWrap);
        row.appendChild(actionCell);

        return row;
    }

    function updateStationCounts(data) {
        var list = Array.isArray(data.station_list) ? data.station_list : [];
        var maxSta = Number(data.max_sta) || 10;
        setText('connect-count', list.length + '台');
        setText('remain-count', Math.max(0, maxSta - list.length) + '台');
        setText('client-count-top', list.length + '台');
        return list;
    }

    function renderStationList(data) {
        if (stationEditing) {
            /* Do not destroy the row the administrator is editing. */
            return;
        }
        var body = CPE.byId('tbody');
        if (!body) {
            return;
        }
        while (body.firstChild) {
            body.removeChild(body.firstChild);
        }
        stationList = updateStationCounts(data);

        stationList.forEach(function (station, index) {
            body.appendChild(renderStationRow(station, index));
        });
    }

    /* Fetch the counts once at page load so the network tab shows meaningful
     * client numbers without starting the (hidden-tab) polling. */
    function loadStationCounts() {
        CPE.getJson(STATION_URL).then(function (data) {
            updateStationCounts(data);
        }).catch(function () {
            /* 401 already redirected to the login page. */
        });
    }

    function refreshStations() {
        if (stationTimer) {
            clearTimeout(stationTimer);
            stationTimer = null;
        }
        if (stationEditing) {
            stationTimer = setTimeout(refreshStations, 4000);
            return;
        }
        CPE.getJson(STATION_URL).then(function (data) {
            renderStationList(data);
        }).catch(function () {
            /* 401 already redirected to the login page. */
        }).then(function () {
            stationTimer = setTimeout(refreshStations, 4000);
        });
    }

    function renameStation(index, name, input, button) {
        var station = stationList[index];
        if (!station) {
            return;
        }
        if (name.length === 0 || name.length > 32) {
            window.alert('主机名长度必须为 1~32 个字符');
            return;
        }
        button.disabled = true;
        CPE.postJson(CHANGE_NAME_URL, {
            name_str: name,
            mac_str: station.mac_str
        }).then(function () {
            input.disabled = true;
            input.value = name;
            button.value = '编辑';
        }).catch(function (error) {
            window.alert(CPE.errorText(error));
        }).then(function () {
            stationEditing = false;
            button.disabled = false;
            refreshStations();
        });
    }

    function deleteStation(index) {
        var station = stationList[index];
        if (!station) {
            return;
        }
        stationEditing = false;
        CPE.postJson(DELETE_DEVICE_URL, {
            mac_str: station.mac_str
        }).then(function () {
            refreshStations();
        }).catch(function (error) {
            window.alert(CPE.errorText(error));
            refreshStations();
        });
    }

    /* Map the modem's signal reading to the five-bar icon. RSRP is the LTE
     * quality metric, so prefer it; RSSI is only a fallback for modems that
     * report CSQ without QCSQ. */
    function signalLevel(status) {
        if (status.qcsq_valid && typeof status.rsrp === 'number') {
            if (status.rsrp >= -85) {
                return 5;
            }
            if (status.rsrp >= -95) {
                return 4;
            }
            if (status.rsrp >= -105) {
                return 3;
            }
            if (status.rsrp >= -115) {
                return 2;
            }
            return 1;
        }
        if (status.rssi_valid && typeof status.rssi === 'number') {
            if (status.rssi >= -65) {
                return 5;
            }
            if (status.rssi >= -75) {
                return 4;
            }
            if (status.rssi >= -85) {
                return 3;
            }
            if (status.rssi >= -95) {
                return 2;
            }
            return 1;
        }
        return 0;
    }

    var SIGNAL_LABELS = ['信号强度未知', '信号很弱', '信号较弱', '信号一般', '信号良好', '信号很强'];

    function renderSignalIcon(status) {
        var icon = CPE.byId('signal-icon');
        if (!icon) {
            return;
        }
        var level = signalLevel(status);
        /* SVG elements expose className as a read-only SVGAnimatedString. */
        icon.setAttribute('class', level === 0 ? 'signal-icon' : 'signal-icon level-' + level);
        icon.setAttribute('aria-label', SIGNAL_LABELS[level]);
    }

    function renderUptime() {
        if (!uptimeSynced || statusStale) {
            return;
        }
        var elapsedMs = performance.now() - uptimeBaseMs;
        setText('connect-time', formatDuration(uptimeBaseS + elapsedMs / 1000));
        /* Re-arm against the next whole second instead of using a fixed 1 s
         * interval: interval drift can skip or repeat a second, which reads as
         * the clock stuttering. */
        if (uptimeTimer) {
            clearTimeout(uptimeTimer);
        }
        uptimeTimer = setTimeout(renderUptime, 1000 - elapsedMs % 1000 + 15);
    }

    function syncUptime(status) {
        var serverS = Number(status.uptime_s) || 0;
        var now = performance.now();
        var localS = uptimeSynced
            ? uptimeBaseS + (now - uptimeBaseMs) / 1000
            : serverS;
        if (!uptimeSynced || Math.abs(serverS - localS) >= UPTIME_RESYNC_THRESHOLD_S) {
            uptimeBaseS = serverS;
            uptimeBaseMs = now;
            uptimeSynced = true;
        }
        renderUptime();
    }

    function renderStatus(status) {
        renderSignalIcon(status);
        syncUptime(status);
        setText('status-state', status.state_text || '未知');
        setText('status-usb', status.usb_connected ? '已连接' : '未连接');
        setText('status-link', status.link_up ? '已连接' : '未连接');
        setText('status-ip', (status.got_ip && status.ip) ? status.ip : '未获取');
        setText('status-rssi', status.rssi_valid ? (status.rssi + ' dBm') : '未知');
        setText('status-rsrp', status.qcsq_valid ? (status.rsrp + ' dBm') : '未知');
        setText('status-rsrq', status.qcsq_valid ? (status.rsrq + ' dB') : '未知');
        setText('status-sinr', status.qcsq_valid ? (status.sinr + ' dB') : '未知');
        setText('status-carrier', status.carrier || '未知');
        setText('status-registered', status.sim_valid
            ? (status.modem_registered ? '已注册' : '未注册')
            : '未知');
        setText('status-ps', status.ps_valid
            ? (status.ps_attached ? '已附着' : '未附着')
            : '未知');
        setText('status-pdp', status.pdp_valid
            ? (status.pdp_active ? '已激活' : '未激活')
            : '未知');
        setText('status-temp', status.modem_temp_valid
            ? (status.modem_temp + ' °C')
            : '未知');
        setText('status-modem-model', status.modem_model || '未知');
        setText('status-modem-fw', status.modem_fw || '未知');
        setText('status-imei', status.imei || '未知');
        setText('status-sim', status.sim_valid
            ? (status.sim_inserted ? '已安装' : '未安装')
            : '未知');
        /* Only meaningful when the modem reported a card as present. */
        setText('status-phone', (status.sim_valid && !status.sim_inserted)
            ? '--'
            : (status.phone_number || '未知'));
        setText('status-recovery', String(status.recovery_attempts || 0));
        setText('status-boot', String(status.boot_count || 0));
        setText('status-device-name', status.device_name || '-');
        setText('status-last-error', status.last_error || '无');

        /* AT telemetry tells whether the WAN can actually work: got_ip alone
         * is misleading, the modem's DHCP server may answer without a SIM. */
        var simKnown = status.sim_valid === true;
        var internetState;
        if (simKnown && !status.sim_inserted) {
            internetState = '未安装 SIM 卡';
        } else if (simKnown && !status.modem_registered) {
            internetState = '未注册移动网络';
        } else if (status.got_ip) {
            internetState = '移动数据已连接';
        } else if (status.link_up || status.usb_connected) {
            internetState = '正在获取网络…';
        } else {
            internetState = '未检测到 4G 模块';
        }
        setText('internet-state', internetState);

        var apSsid = status.ap && typeof status.ap.ssid === 'string' ? status.ap.ssid : '';
        setText('wifi-state', apSsid ? ('2.4 GHz Wi-Fi 正常（' + apSsid + '）') : '2.4 GHz Wi-Fi 正常');

        var banner = CPE.byId('status-banner');
        if (banner) {
            if (status.state === STATE_FAULT) {
                banner.textContent = '当前处于故障状态：4G 链路未就绪，系统会自动重试。' +
                    '您仍可以通过本页面修改 WLAN 配置或恢复出厂设置。';
                banner.className = 'status-banner status-banner-warn';
            } else if (simKnown && !status.sim_inserted) {
                banner.textContent = '未检测到 SIM 卡，4G 网络不可用。插入 SIM 卡后系统会自动恢复。';
                banner.className = 'status-banner status-banner-warn';
            } else if (simKnown && !status.modem_registered) {
                banner.textContent = 'SIM 卡已安装，但尚未注册到移动网络，正在等待网络注册。';
                banner.className = 'status-banner status-banner-info';
            } else if (!status.link_up) {
                banner.textContent = '正在等待 4G 网络就绪，本页面在链路恢复前仍可正常使用。';
                banner.className = 'status-banner status-banner-info';
            } else {
                banner.textContent = '';
                banner.className = 'status-banner';
            }
        }
    }

    /* The last successful reading is kept on screen, but it must be labelled:
     * otherwise a frozen page looks live (and the local uptime keeps counting
     * even though the device was never contacted again). */
    function showStaleStatus() {
        var banner = CPE.byId('status-banner');
        if (!banner) {
            return;
        }
        var lastSeen = lastStatusAt
            ? '，当前显示的是 ' + formatClock(lastStatusAt) + ' 的数据'
            : '';
        banner.textContent = '与设备失去连接，正在重试' + lastSeen + '；恢复后会自动更新。';
        banner.className = 'status-banner status-banner-warn';
    }

    function refreshStatus() {
        if (statusTimer) {
            clearTimeout(statusTimer);
            statusTimer = null;
        }
        CPE.getStatus().then(function (status) {
            statusStale = false;
            lastStatusAt = new Date();
            renderStatus(status);
        }).catch(function () {
            /* 401 already redirected to the login page. */
            if (statusStale) {
                return;
            }
            statusStale = true;
            if (uptimeTimer) {
                clearTimeout(uptimeTimer);
                uptimeTimer = null;
            }
            showStaleStatus();
        }).then(function () {
            statusTimer = setTimeout(refreshStatus, 5000);
        });
    }

    function switchTab(showNetwork) {
        if (!CPE.showTab(showNetwork, 'networkShow', 'customerShow',
                         'tab-network', 'tab-clients')) {
            return;
        }
        if (showNetwork) {
            /* Leaving the client tab abandons a pending edit and stops polling. */
            stationEditing = false;
            if (stationTimer) {
                clearTimeout(stationTimer);
                stationTimer = null;
            }
        } else {
            refreshStations();
        }
    }

    function bindActions() {
        var changePassword = CPE.byId('btn-change-password');
        if (changePassword) {
            changePassword.addEventListener('click', function () {
                CPE.openPasswordModal(false).catch(function () {});
            });
        }
        var factoryReset = CPE.byId('btn-factory-reset');
        if (factoryReset) {
            factoryReset.addEventListener('click', function () {
                CPE.factoryReset().catch(function () {});
            });
        }
    }

    function init() {
        /* Without a CSRF token this browser has no session to probe; go to the
         * login page directly instead of triggering a 401 on /api/status. */
        if (!CPE.getCsrf()) {
            window.location.href = '/login.html';
            return;
        }
        CPE.bindMenu();
        bindActions();
        /* Background tabs throttle timers; catch up as soon as the page is
         * visible again instead of leaving a stale time on screen. */
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) {
                renderUptime();
            }
        });
        var networkTab = CPE.byId('tab-network');
        var clientTab = CPE.byId('tab-clients');
        if (networkTab) {
            networkTab.addEventListener('click', function () {
                switchTab(true);
            });
        }
        if (clientTab) {
            clientTab.addEventListener('click', function () {
                switchTab(false);
            });
        }

        CPE.bootstrap().then(function (status) {
            renderStatus(status);
            switchTab(true);
            loadStationCounts();
            refreshStatus();
        }).catch(function () {
            /* Forced password change already redirected to the login page. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
