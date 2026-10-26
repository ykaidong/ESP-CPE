/*
 * System settings page: scheduled maintenance, admin password, reboot and
 * factory reset.
 *
 * The maintenance countdown is polled here instead of on the status page, so
 * the 30 s timer only runs while this page is open.
 */
'use strict';

(function () {
    var MAINT_URL = '/api/system/maintenance';
    /* Keep in sync with CPE_MAINT_PERIOD_MAX_MIN (cpe_maintenance.h). */
    var MAINT_PERIOD_MAX_MIN = 43200;
    /* The input fields are only filled when the page loads or after saving, so
     * a poll never overwrites typing. */
    var MAINT_POLL_MS = 30000;

    var maintTimer = null;
    var maintInFlight = false;
    var maintStale = false;

    function showBanner(text, isError) {
        var node = CPE.byId('system-banner');
        if (!node) {
            return;
        }
        node.textContent = text || '';
        if (!text) {
            node.className = 'status-banner';
        } else if (isError) {
            node.className = 'status-banner status-banner-warn';
        } else {
            node.className = 'status-banner status-banner-info';
        }
    }

    function formatRemain(seconds) {
        var total = Math.max(0, Math.floor(seconds));
        var hours = Math.floor(total / 3600);
        var minutes = Math.floor((total % 3600) / 60);
        if (hours > 0) {
            return hours + ' 小时 ' + minutes + ' 分钟';
        }
        if (minutes > 0) {
            return minutes + ' 分钟';
        }
        return total + ' 秒';
    }

    function formatPeriod(minutes) {
        if (minutes <= 0) {
            return '已关闭';
        }
        if (minutes % 1440 === 0) {
            return '每 ' + (minutes / 1440) + ' 天';
        }
        if (minutes % 60 === 0) {
            return '每 ' + (minutes / 60) + ' 小时';
        }
        if (minutes > 60) {
            return '每 ' + Math.floor(minutes / 60) + ' 小时 ' + (minutes % 60) + ' 分钟';
        }
        return '每 ' + minutes + ' 分钟';
    }

    function renderMaintenance(data, fillInputs) {
        var ecmPeriod = Number(data.ecm_period_min) || 0;
        var rebootPeriod = Number(data.reboot_period_min) || 0;
        var nextEcm = Number(data.next_ecm_in_s);
        var nextReboot = Number(data.next_reboot_in_s);
        var count = Number(data.sched_reboot_count) || 0;

        if (fillInputs) {
            var ecmInput = CPE.byId('maint-ecm');
            var rebootInput = CPE.byId('maint-reboot');
            if (ecmInput) {
                ecmInput.value = String(ecmPeriod);
            }
            if (rebootInput) {
                rebootInput.value = String(rebootPeriod);
            }
        }

        CPE.setText('maint-ecm-value', formatPeriod(ecmPeriod));
        CPE.setText('maint-ecm-hint', ecmPeriod > 0
            ? (nextEcm >= 0 ? '下次：' + formatRemain(nextEcm) + '后' : '正在排期')
            : '不重建 ECM 网卡');

        CPE.setText('maint-reboot-value', formatPeriod(rebootPeriod));
        CPE.setText('maint-reboot-hint', rebootPeriod > 0
            ? (nextReboot >= 0 ? '下次：' + formatRemain(nextReboot) + '后' : '正在排期')
            : '不自动重启设备');

        if (data.reboot_frozen) {
            CPE.setText('maint-state', '已冻结');
            CPE.setText('maint-state-hint', '连续 ' + count + ' 次重启未恢复正常');
        } else if (count > 0) {
            CPE.setText('maint-state', '恢复观察中');
            CPE.setText('maint-state-hint', '已连续重启 ' + count + ' 次');
        } else {
            CPE.setText('maint-state', '正常');
            CPE.setText('maint-state-hint', '无异常重启记录');
        }
    }

    function loadMaintenance(fillInputs) {
        if (maintInFlight) {
            return Promise.resolve(null);
        }
        maintInFlight = true;
        return CPE.getJson(MAINT_URL).then(function (data) {
            maintInFlight = false;
            renderMaintenance(data, fillInputs);
            if (maintStale) {
                maintStale = false;
                showBanner('');
            }
            return data;
        }).catch(function () {
            maintInFlight = false;
            /* A 401 already redirected to the login page. Everything else means
             * the readings on screen can no longer be trusted, so say so. */
            if (!maintStale) {
                maintStale = true;
                showBanner('与设备失去连接，正在重试；当前显示的是上次成功读取的数据。', true);
            }
            return null;
        });
    }

    function schedulePoll() {
        if (maintTimer) {
            clearTimeout(maintTimer);
        }
        maintTimer = setTimeout(function () {
            loadMaintenance(false).then(schedulePoll);
        }, MAINT_POLL_MS);
    }

    function refreshNow() {
        if (maintTimer) {
            clearTimeout(maintTimer);
            maintTimer = null;
        }
        loadMaintenance(false).then(schedulePoll);
    }

    function saveMaintenance() {
        var ecmInput = CPE.byId('maint-ecm');
        var rebootInput = CPE.byId('maint-reboot');
        if (!ecmInput || !rebootInput) {
            return;
        }
        var ecmPeriod = parseInt(ecmInput.value, 10);
        var rebootPeriod = parseInt(rebootInput.value, 10);
        if (isNaN(ecmPeriod) || ecmPeriod < 0 || ecmPeriod > MAINT_PERIOD_MAX_MIN ||
                isNaN(rebootPeriod) || rebootPeriod < 0 || rebootPeriod > MAINT_PERIOD_MAX_MIN) {
            showBanner('周期必须为 0~43200 的整数（分钟，0 表示关闭）', true);
            return;
        }
        var saveButton = CPE.byId('btn-save-maint');
        if (saveButton) {
            saveButton.disabled = true;
        }
        CPE.postJson(MAINT_URL, {
            ecm_period_min: ecmPeriod,
            reboot_period_min: rebootPeriod
        }).then(function (data) {
            /* Reflect what the device actually stored and when it will run. */
            renderMaintenance(data, true);
            showBanner('维护周期已保存，倒计时从当前时刻重新开始。', false);
        }).catch(function (error) {
            if (error && error.message === 'network_error') {
                showBanner('保存请求超时，请稍后刷新页面确认是否已生效。', true);
                return;
            }
            showBanner('保存失败：' + CPE.errorText(error), true);
        }).then(function () {
            if (saveButton) {
                saveButton.disabled = false;
            }
        });
    }

    function reboot() {
        if (!window.confirm('确定要重启设备吗？重启期间 Wi-Fi 会断开约 20 秒。')) {
            return;
        }
        var rebootButton = CPE.byId('btn-reboot');
        if (rebootButton) {
            rebootButton.disabled = true;
        }
        CPE.postJson('/api/system/reboot', {}).then(function () {
            /* Stay disabled: the page is about to lose the device. */
            showBanner('设备正在重启，请等待约 20 秒后刷新页面。', false);
        }).catch(function (error) {
            /* The device can drop the connection before replying; either way
             * the reboot has been accepted. */
            if (error && error.message === 'network_error') {
                showBanner('设备正在重启，请等待约 20 秒后刷新页面。', false);
                return;
            }
            if (rebootButton) {
                rebootButton.disabled = false;
            }
            showBanner('重启失败：' + CPE.errorText(error), true);
        });
    }

    function bindActions() {
        var changePassword = CPE.byId('btn-change-password');
        if (changePassword) {
            changePassword.addEventListener('click', function () {
                CPE.openPasswordModal(false).catch(function () {});
            });
        }
        var rebootButton = CPE.byId('btn-reboot');
        if (rebootButton) {
            rebootButton.addEventListener('click', reboot);
        }
        var factoryReset = CPE.byId('btn-factory-reset');
        if (factoryReset) {
            factoryReset.addEventListener('click', function () {
                CPE.factoryReset().catch(function () {});
            });
        }
        var saveMaint = CPE.byId('btn-save-maint');
        if (saveMaint) {
            saveMaint.addEventListener('click', saveMaintenance);
        }
    }

    function init() {
        CPE.bindMenu();
        bindActions();
        /* Background tabs throttle timers; catch up as soon as the page is
         * visible again instead of leaving stale countdowns on screen. */
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) {
                refreshNow();
            }
        });
        CPE.guardSession().then(function () {
            loadMaintenance(true).then(schedulePoll);
        }).catch(function () {
            /* Forced password change or missing session: already redirected. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
