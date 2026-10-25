/*
 * Shared management console helpers for the ESP-CPE web UI.
 *
 *  - session requests carry the X-CSRF-Token header (the session cookie itself
 *    is HttpOnly and never visible to JavaScript),
 *  - a 401 response clears the local CSRF token and redirects to the login
 *    page,
 *  - user controlled data is always rendered with textContent/setAttribute,
 *    never with innerHTML,
 *  - the forced first-login password change is implemented as a modal that
 *    cannot be dismissed.
 */
'use strict';

var CPE = (function () {
    var CSRF_STORAGE_KEY = 'cpe_csrf_token';
    var CSRF_COOKIE_NAME = 'CPE_CSRF';
    var csrfToken = null;

    /* Every console request is small and answered within milliseconds. A hard
     * timeout matters when the SoftAP restarts (SSID/password/channel change)
     * or the device reboots: the TCP connection is then half-open, the browser
     * never sees a FIN/RST and keeps the socket in its per-origin pool. Six
     * such sockets block every later request, including a page reload, which
     * looks exactly like "the console is unreachable" while the device is fine. */
    var REQUEST_TIMEOUT_MS = 8000;

    /* The server stores the CSRF token in a non-HttpOnly SameSite=Strict
     * cookie, so it is available after a reload and in new tabs. */
    function readCookie(name) {
        try {
            var parts = document.cookie ? document.cookie.split(';') : [];
            for (var i = 0; i < parts.length; i++) {
                var item = parts[i].replace(/^\s+/, '');
                if (item.indexOf(name + '=') === 0) {
                    return decodeURIComponent(item.substring(name.length + 1));
                }
            }
        } catch (e) {
            /* ignore */
        }
        return null;
    }

    function storageGet(key) {
        try {
            var value = window.sessionStorage.getItem(key);
            if (value !== null) {
                return value;
            }
        } catch (e) {
            /* fall through to window.name fallback */
        }
        /* Privacy modes may block sessionStorage; window.name survives the
         * login -> index navigation in the same tab and is never sent to the
         * server. It only carries the CSRF token, which is not a secret. */
        try {
            if (window.name && window.name.indexOf('cpe:') === 0) {
                var store = JSON.parse(window.name.slice(4));
                return store[key] || null;
            }
        } catch (e) {
            /* ignore */
        }
        return null;
    }

    function storageSet(key, value) {
        try {
            if (value === null || value === undefined) {
                window.sessionStorage.removeItem(key);
            } else {
                window.sessionStorage.setItem(key, value);
            }
            return;
        } catch (e) {
            /* fall through to window.name fallback */
        }
        try {
            var store = {};
            if (window.name && window.name.indexOf('cpe:') === 0) {
                try {
                    store = JSON.parse(window.name.slice(4));
                } catch (e2) {
                    store = {};
                }
            }
            if (value === null || value === undefined) {
                delete store[key];
            } else {
                store[key] = value;
            }
            window.name = 'cpe:' + JSON.stringify(store);
        } catch (e3) {
            /* private mode and window.name blocked: in-memory copy only */
        }
    }

    function setCsrf(token) {
        csrfToken = token || null;
        storageSet(CSRF_STORAGE_KEY, csrfToken);
    }

    function getCsrf() {
        if (!csrfToken) {
            csrfToken = readCookie(CSRF_COOKIE_NAME) || storageGet(CSRF_STORAGE_KEY);
        }
        return csrfToken;
    }

    function clearCsrf() {
        csrfToken = null;
        storageSet(CSRF_STORAGE_KEY, null);
        /* The server expires the cookie on logout/password change; expire it
         * here too so a stale value cannot be picked up again. */
        try {
            document.cookie = CSRF_COOKIE_NAME + '=; Path=/; Max-Age=0; SameSite=Strict';
        } catch (e) {
            /* ignore */
        }
    }

    function isLoginPage() {
        return window.location.pathname === '/login.html';
    }

    function redirectToLogin() {
        if (!isLoginPage()) {
            window.location.href = '/login.html';
        }
    }

    function request(method, url, body, timeoutMs) {
        return new Promise(function (resolve, reject) {
            var xhr = new XMLHttpRequest();
            xhr.open(method, url, true);
            xhr.timeout = timeoutMs || REQUEST_TIMEOUT_MS;
            xhr.setRequestHeader('Accept', 'application/json');
            if (body !== undefined && body !== null) {
                xhr.setRequestHeader('Content-Type', 'application/json');
            }
            var token = getCsrf();
            if (token) {
                xhr.setRequestHeader('X-CSRF-Token', token);
            }
            xhr.onreadystatechange = function () {
                if (xhr.readyState !== 4) {
                    return;
                }
                if (xhr.status === 0) {
                    /* Connection reset before a response (typical when the AP
                     * is reconfigured or the device reboots). Report it as a
                     * network event instead of the misleading "http_0". */
                    reject(new Error('network_error'));
                    return;
                }
                if (xhr.status === 401 && url !== '/api/login') {
                    clearCsrf();
                    redirectToLogin();
                    reject(new Error('unauthorized'));
                    return;
                }
                var data = {};
                if (xhr.responseText) {
                    try {
                        data = JSON.parse(xhr.responseText);
                    } catch (e) {
                        data = { status: 'error', error: 'bad_response' };
                    }
                }
                if (xhr.status >= 200 && xhr.status < 300) {
                    resolve(data);
                    return;
                }
                if (xhr.status === 403 && data.error === 'csrf_invalid') {
                    /* The CSRF token is kept per browser tab; a new tab or a
                     * restored session has to log in again. */
                    clearCsrf();
                    redirectToLogin();
                }
                var error = new Error(data.error || ('http_' + xhr.status));
                error.status = xhr.status;
                error.code = data.error || null;
                reject(error);
            };
            xhr.onerror = function () {
                reject(new Error('network_error'));
            };
            xhr.ontimeout = function () {
                /* Aborting on timeout also closes the socket, so a dead
                 * connection cannot keep occupying a browser connection slot. */
                reject(new Error('network_error'));
            };
            xhr.send(body === undefined || body === null ? null : JSON.stringify(body));
        });
    }

    function getJson(url, timeoutMs) {
        return request('GET', url, undefined, timeoutMs);
    }

    function postJson(url, body, timeoutMs) {
        return request('POST', url, body || {}, timeoutMs);
    }

    function getStatus() {
        return getJson('/api/status');
    }

    function logout() {
        return postJson('/api/logout', {}).then(function () {
            clearCsrf();
            redirectToLogin();
        });
    }

    function factoryReset() {
        if (!window.confirm('确定要恢复出厂设置吗？')) {
            return Promise.reject(new Error('cancelled'));
        }
        if (!window.confirm('恢复出厂会清除除 Wi-Fi 名称/口令外的全部配置并重启设备，确定继续吗？')) {
            return Promise.reject(new Error('cancelled'));
        }
        return postJson('/api/factory_reset', {}).then(function () {
            clearCsrf();
            window.alert('已恢复出厂设置（Wi-Fi 名称与口令保留），设备正在重启，请重新登录。');
        });
    }

    var ERROR_TEXT = {
        invalid_credentials: '用户名或密码错误',
        locked_out: '登录失败次数过多，请稍后再试',
        unauthorized: '登录已过期，请重新登录',
        weak_password: '新密码长度必须为 8~63 个字符',
        password_change_required: '请先修改初始密码',
        invalid_ssid: 'WLAN 名称必须为 1~32 个可见字符',
        invalid_password: '密码长度必须为 8~63 个字符',
        unsupported_auth_mode: '仅支持 WPA2-PSK 加密方式',
        invalid_device_name: '设备名称必须为 1~32 个可见字符',
        invalid_max_sta: '最大客户端数量必须为 1~10',
        invalid_channel: '信道必须为 1~13',
        invalid_bandwidth: '带宽必须为 20 或 40 MHz',
        invalid_station_name: '主机名只能包含字母、数字、空格以及 _ - . @ ( )',
        invalid_mac: 'MAC 地址格式不正确',
        invalid_maintenance_period: '定时维护周期必须为 0~43200 的整数（分钟，0 表示关闭）',
        invalid_request_body: '请求参数格式不正确',
        no_fields: '请至少填写一个周期',
        station_not_found: '该客户端当前不在线',
        csrf_invalid: '会话校验失败，请刷新页面后重试',
        persist_failed: '设置保存失败，请重试',
        config_unavailable: '配置不可用，请稍后重试',
        deauth_failed: '断开客户端失败，请重试',
        factory_reset_failed: '恢复出厂设置失败，请重试',
        cross_origin_rejected: '请求来源校验失败',
        body_too_large: '请求内容过大',
        body_timeout: '读取请求内容超时，请重试',
        empty_body: '请求内容为空',
        invalid_json: '请求格式不正确',
        invalid_body: '请求内容不合法',
        unexpected_body: '请求内容不合法',
        unsupported_media_type: '不支持的请求内容类型',
        invalid_request: '请求参数不正确',
        invalid_path: '请求路径不合法',
        uri_too_long: '请求地址过长',
        not_found: '请求的资源不存在',
        out_of_memory: '设备内存不足，请稍后重试',
        internal_error: '设备内部错误，请重试',
        invalid_firmware_image: '所选文件不是有效的 ESP32 固件',
        image_too_large: '固件大小超过 OTA 分区容量',
        upload_interrupted: '上传中断，请重试',
        upload_timeout: '上传超时，请重试',
        ota_busy: '已有升级任务正在进行，请稍后再试',
        pending_verify: '当前固件尚未完成启动验证，请等待约 1 分钟后再试',
        ota_begin_failed: '无法开始升级，请重试',
        ota_write_failed: '固件写入失败，请重试',
        ota_finalize_failed: '固件写入完成但切换启动分区失败，请重试',
        ota_info_failed: '读取固件信息失败',
        sms_not_supported: '当前模组不支持短信功能',
        sms_bad_number: '号码格式不正确（可含 +，3~20 位数字）',
        sms_text_too_long: '短信内容过长（ASCII 最多 160 字符，中文最多 70 字）',
        sms_rate_limited: '发送过于频繁，请稍后再试',
        sms_send_failed: '短信发送失败，请检查 SIM 卡与短信中心',
        sms_send_timeout: '短信发送超时，请重试',
        sms_no_prompt: '模组未就绪，无法发送短信',
        network_error: '网络连接失败，设备可能正在重启',
        bad_response: '设备响应异常'
    };

    function errorText(error) {
        if (!error) {
            return '操作失败，请重试';
        }
        return ERROR_TEXT[error.code] || ERROR_TEXT[error.message] || '操作失败，请重试';
    }

    function byId(id) {
        return document.getElementById(id);
    }

    /* Collapsible left menu, shared by every console page. */
    function bindMenu() {
        var menuButton = byId('menuBtn');
        var menu = byId('leftMenu');
        if (menuButton && menu) {
            menuButton.addEventListener('click', function () {
                /* The menu starts hidden via CSS (display:none), not an inline
                 * style, so check the computed value instead of style.display. */
                var open = window.getComputedStyle(menu).display === 'none';
                menu.style.display = open ? 'block' : 'none';
                menuButton.className = open ? 'header-menu menu-open' : 'header-menu';
            });
        }

        var logoutButton = byId('btn-logout');
        if (logoutButton) {
            logoutButton.addEventListener('click', function () {
                /* Never fail silently: the user must know whether they are
                 * still logged in. */
                logout().catch(function (error) {
                    window.alert(errorText(error));
                });
            });
        }
    }

    /* Show one of two tab panels and mark the matching tab as active. Returns
     * false when any of the elements is missing. Page-specific side effects
     * (polling, pending edits) stay in the caller. */
    function showTab(showFirst, firstPanelId, secondPanelId, firstTabId, secondTabId) {
        var firstPanel = byId(firstPanelId);
        var secondPanel = byId(secondPanelId);
        var firstTab = byId(firstTabId);
        var secondTab = byId(secondTabId);
        if (!firstPanel || !secondPanel || !firstTab || !secondTab) {
            return false;
        }
        firstPanel.style.display = showFirst ? 'block' : 'none';
        secondPanel.style.display = showFirst ? 'none' : 'block';
        firstTab.className = showFirst ? 'header-title-one active-title' : 'header-title-one';
        secondTab.className = showFirst ? 'header-title-two' : 'header-title-two active-title';
        return true;
    }

    function createElement(tag, props) {
        var node = document.createElement(tag);
        if (props) {
            Object.keys(props).forEach(function (key) {
                var value = props[key];
                if (key === 'text') {
                    node.textContent = value;
                } else if (key === 'className') {
                    node.className = value;
                } else if (key === 'onclick') {
                    node.addEventListener('click', value);
                } else if (value !== null && value !== undefined) {
                    node.setAttribute(key, value);
                }
            });
        }
        return node;
    }

    /* Password change modal, shared by the forced first-login flow and the
     * voluntary "change password" action in the system panel. */
    function openPasswordModal(mandatory) {
        return new Promise(function (resolve, reject) {
            var overlay = createElement('div', { className: 'modal-overlay' });
            var panel = createElement('div', { className: 'modal-panel' });
            var title = createElement('div', {
                className: 'modal-title',
                text: mandatory ? '首次登录，请修改初始密码' : '修改管理员密码'
            });
            var hint = createElement('div', {
                className: 'modal-hint',
                text: mandatory
                    ? '为保障设备安全，必须修改初始密码后才能继续使用管理后台。'
                    : '密码长度必须为 8~63 个字符。'
            });
            var oldInput = createElement('input', {
                type: 'password',
                className: 'modal-input',
                placeholder: '当前密码',
                autocomplete: 'current-password',
                maxlength: '63'
            });
            var newInput = createElement('input', {
                type: 'password',
                className: 'modal-input',
                placeholder: '新密码（8~63 位）',
                autocomplete: 'new-password',
                maxlength: '63'
            });
            var confirmInput = createElement('input', {
                type: 'password',
                className: 'modal-input',
                placeholder: '确认新密码',
                autocomplete: 'new-password',
                maxlength: '63'
            });
            var errorBox = createElement('div', { className: 'modal-error' });
            var actions = createElement('div', { className: 'modal-actions' });
            var submit = createElement('button', { className: 'save-btn', type: 'button', text: '确认修改' });

            function close() {
                if (overlay.parentNode) {
                    overlay.parentNode.removeChild(overlay);
                }
            }

            function finish() {
                close();
                resolve();
            }

            function submitChange() {
                errorBox.textContent = '';
                var oldPassword = oldInput.value;
                var newPassword = newInput.value;
                if (newPassword.length < 8 || newPassword.length > 63) {
                    errorBox.textContent = '新密码长度必须为 8~63 个字符';
                    return;
                }
                if (newPassword !== confirmInput.value) {
                    errorBox.textContent = '两次输入的新密码不一致';
                    return;
                }
                submit.disabled = true;
                postJson('/api/password', {
                    old_password: oldPassword,
                    new_password: newPassword
                }).then(function () {
                    clearCsrf();
                    window.alert('密码修改成功，请使用新密码重新登录。');
                    window.location.href = '/login.html';
                    finish();
                }).catch(function (error) {
                    submit.disabled = false;
                    errorBox.textContent = errorText(error);
                });
            }

            submit.addEventListener('click', submitChange);
            [oldInput, newInput, confirmInput].forEach(function (input) {
                input.addEventListener('keydown', function (event) {
                    if (event.key === 'Enter') {
                        submitChange();
                    }
                });
            });

            actions.appendChild(submit);
            if (!mandatory) {
                var cancel = createElement('button', { className: 'cancel-btn', type: 'button', text: '取消' });
                cancel.addEventListener('click', function () {
                    close();
                    reject(new Error('cancelled'));
                });
                actions.appendChild(cancel);
            }

            panel.appendChild(title);
            panel.appendChild(hint);
            panel.appendChild(oldInput);
            panel.appendChild(newInput);
            panel.appendChild(confirmInput);
            panel.appendChild(errorBox);
            panel.appendChild(actions);
            overlay.appendChild(panel);
            document.body.appendChild(overlay);
            oldInput.focus();
        });
    }

    /* Fetch the status once; force the password change when the server says
     * the initial password is still in use. */
    function bootstrap() {
        return getStatus().then(function (status) {
            if (status.must_change) {
                return openPasswordModal(true).then(function () {
                    throw new Error('password_changed');
                });
            }
            return status;
        });
    }

    return {
        setCsrf: setCsrf,
        getCsrf: getCsrf,
        clearCsrf: clearCsrf,
        getJson: getJson,
        postJson: postJson,
        getStatus: getStatus,
        bootstrap: bootstrap,
        logout: logout,
        factoryReset: factoryReset,
        openPasswordModal: openPasswordModal,
        errorText: errorText,
        createElement: createElement,
        byId: byId,
        bindMenu: bindMenu,
        showTab: showTab
    };
}());
