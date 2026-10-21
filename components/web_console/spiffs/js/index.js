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

    function renderStatus(status) {
        setText('status-state', status.state_text || '未知');
        setText('status-usb', status.usb_connected ? '已连接' : '未连接');
        setText('status-link', status.link_up ? '已连接' : '未连接');
        setText('status-ip', (status.got_ip && status.ip) ? status.ip : '未获取');
        setText('status-rssi', status.rssi_valid ? (status.rssi + ' dBm') : '未知');
        setText('status-carrier', status.carrier || '未知');
        setText('status-registered', status.modem_registered ? '已注册' : '未注册');
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
        setText('connect-time', formatDuration(Number(status.uptime_s) || 0));

        setText('internet-state', status.link_up
            ? '移动数据已连接'
            : (status.usb_connected ? '正在获取网络…' : '未检测到 4G 模块'));

        var apSsid = status.ap && typeof status.ap.ssid === 'string' ? status.ap.ssid : '';
        setText('wifi-state', apSsid ? ('2.4 GHz Wi-Fi 正常（' + apSsid + '）') : '2.4 GHz Wi-Fi 正常');

        var banner = CPE.byId('status-banner');
        if (banner) {
            if (status.state === STATE_FAULT) {
                banner.textContent = '当前处于故障状态：4G 链路未就绪，系统会自动重试。' +
                    '您仍可以通过本页面修改 WLAN 配置或恢复出厂设置。';
                banner.className = 'status-banner status-banner-warn';
            } else if (!status.link_up) {
                banner.textContent = '正在等待 4G 网络就绪，本页面在链路恢复前仍可正常使用。';
                banner.className = 'status-banner status-banner-info';
            } else {
                banner.textContent = '';
                banner.className = 'status-banner';
            }
        }
    }

    function refreshStatus() {
        if (statusTimer) {
            clearTimeout(statusTimer);
            statusTimer = null;
        }
        CPE.getStatus().then(function (status) {
            renderStatus(status);
        }).catch(function () {
            /* 401 already redirected. */
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
        var logout = CPE.byId('btn-logout');
        if (logout) {
            logout.addEventListener('click', function () {
                CPE.logout().catch(function (error) {
                    /* Never fail silently: the user must know whether they are
                     * still logged in. */
                    window.alert(CPE.errorText(error));
                });
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
