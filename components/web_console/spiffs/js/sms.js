/*
 * SMS page. All device data is rendered with textContent only. The page polls
 * /api/sms/list while open; a 501 ("sms_not_supported") disables the form and
 * shows an explanatory banner.
 */
'use strict';

(function () {
    var LIST_URL = '/api/sms/list';
    var SEND_URL = '/api/sms/send';
    var DELETE_URL = '/api/sms/delete';
    var POLL_MS = 5000;
    var timer = null;

    function setBanner(text, cls) {
        var b = CPE.byId('sms-banner');
        if (!b) return;
        b.textContent = text || '';
        b.className = cls ? ('status-banner ' + cls) : 'status-banner';
    }

    function setFormEnabled(enabled) {
        var num = CPE.byId('sms-number');
        var text = CPE.byId('sms-text');
        var btn = CPE.byId('sms-send-btn');
        if (num) num.disabled = !enabled;
        if (text) text.disabled = !enabled;
        if (btn) btn.disabled = !enabled;
    }

    function createCell(text) {
        var td = document.createElement('td');
        td.textContent = text;
        return td;
    }

    function renderList(data) {
        var body = CPE.byId('sms-tbody');
        if (!body) return;
        body.textContent = '';
        var list = Array.isArray(data.messages) ? data.messages : [];
        list.forEach(function (m) {
            var row = document.createElement('tr');
            if (m.unread) row.className = 'sms-unread';
            row.appendChild(createCell(m.from || ''));
            row.appendChild(createCell(m.body || ''));
            row.appendChild(createCell(m.time || ''));

            var action = document.createElement('td');
            var del = document.createElement('button');
            del.type = 'button';
            del.className = 'del-btn';
            del.textContent = '删除';
            del.addEventListener('click', function () { remove(m.index); });
            action.appendChild(del);
            row.appendChild(action);
            body.appendChild(row);
        });
        var empty = CPE.byId('sms-empty');
        if (empty) empty.textContent = list.length === 0 ? '收件箱为空' : '';
    }

    function refresh() {
        CPE.getJson(LIST_URL).then(function (data) {
            setFormEnabled(true);
            setBanner('');
            renderList(data);
        }).catch(function (error) {
            if (error.status === 501 || error.code === 'sms_not_supported') {
                setFormEnabled(false);
                setBanner('当前模组不支持短信功能（4G 数据不受影响）。', 'status-banner-info');
            } else {
                setBanner('读取短信失败，正在重试。', 'status-banner-warn');
            }
        }).then(function () {
            timer = setTimeout(refresh, POLL_MS);
        });
    }

    function remove(index) {
        if (!window.confirm('确定删除这条短信吗？')) return;
        CPE.postJson(DELETE_URL, { index: index }).then(function () {
            refresh();
        }).catch(function (error) {
            window.alert(CPE.errorText(error));
        });
    }

    function send() {
        var num = CPE.byId('sms-number');
        var text = CPE.byId('sms-text');
        if (!num || !text) return;
        if (!num.value || !text.value) {
            setBanner('号码和内容都不能为空。', 'status-banner-warn');
            return;
        }
        var btn = CPE.byId('sms-send-btn');
        if (btn) btn.disabled = true;
        CPE.postJson(SEND_URL, { number: num.value, text: text.value }, 70000).then(function () {
            setBanner('发送成功。', 'status-banner-info');
            text.value = '';
            refresh();
        }).catch(function (error) {
            setBanner(CPE.errorText(error), 'status-banner-warn');
        }).then(function () {
            if (btn) btn.disabled = false;
        });
    }

    function init() {
        if (!CPE.getCsrf()) {
            window.location.href = '/login.html';
            return;
        }
        CPE.bindMenu();
        var btn = CPE.byId('sms-send-btn');
        if (btn) btn.addEventListener('click', send);
        setFormEnabled(false);
        CPE.bootstrap().then(function () {
            refresh();
        }).catch(function () {
            /* Forced password change already redirected to the login page. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
