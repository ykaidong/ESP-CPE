/*
 * SMS page. Device data is rendered with textContent only, never innerHTML.
 * The list is polled while the page is visible; rows are reused between polls
 * so the table no longer flickers, and an in-flight guard stops a slow CMGL
 * from overlapping with the next poll (the AT port rejects concurrent calls).
 */
'use strict';

(function () {
    var LIST_URL = '/api/sms/list';
    var SEND_URL = '/api/sms/send';
    var DELETE_URL = '/api/sms/delete';
    var POLL_MS = 10000;

    var timer = null;
    var inFlight = false;
    var rows = {};

    function byId(id) {
        return CPE.byId(id);
    }

    function setBanner(text, cls) {
        var b = byId('sms-banner');
        if (!b) return;
        b.textContent = text || '';
        b.className = cls ? ('status-banner ' + cls) : 'status-banner';
    }

    function setFormEnabled(enabled) {
        var num = byId('sms-number');
        var text = byId('sms-text');
        var btn = byId('sms-send-btn');
        if (num) num.disabled = !enabled;
        if (text) text.disabled = !enabled;
        if (btn) btn.disabled = !enabled;
    }

    /* Rough client-side length hint; the server is authoritative. JS strings are
     * UTF-16, so text.length already equals the UCS2 unit count the server
     * enforces (160 for pure ASCII, 70 code units otherwise). */
    function updateCount() {
        var text = byId('sms-text');
        var count = byId('sms-count');
        if (!text || !count) return;

        var value = text.value;
        var ascii = true;
        for (var i = 0; i < value.length; i++) {
            if (value.charCodeAt(i) >= 128) {
                ascii = false;
                break;
            }
        }
        var limit = ascii ? 160 : 70;
        var units = value.length;
        count.textContent = units + ' / ' + limit;
        count.className = units > limit ? 'sms-count over' : 'sms-count';
    }

    function makeCell(text, cls) {
        var td = document.createElement('td');
        if (cls) td.className = cls;
        td.textContent = text;
        return td;
    }

    function renderList(data) {
        var body = byId('sms-tbody');
        if (!body) return;

        var list = Array.isArray(data.messages) ? data.messages : [];
        var seen = {};
        var order = [];

        list.forEach(function (m) {
            var key = String(m.index);
            seen[key] = true;

            var row = rows[key];
            if (!row) {
                row = document.createElement('tr');
                row.appendChild(makeCell(m.from || '', 'sms-from'));
                row.appendChild(makeCell(m.body || '', 'sms-body'));
                row.appendChild(makeCell(m.time || '', 'sms-time'));

                var opCell = document.createElement('td');
                opCell.className = 'sms-op';
                var del = document.createElement('button');
                del.type = 'button';
                del.className = 'sms-del';
                del.textContent = '删除';
                del.addEventListener('click', function () { remove(m.index); });
                opCell.appendChild(del);
                row.appendChild(opCell);

                rows[key] = row;
            } else {
                row.children[0].textContent = m.from || '';
                row.children[1].textContent = m.body || '';
                row.children[2].textContent = m.time || '';
            }
            row.className = m.unread ? 'sms-unread' : '';
            order.push(row);
        });

        Object.keys(rows).forEach(function (k) {
            if (!seen[k]) {
                var r = rows[k];
                if (r.parentNode) r.parentNode.removeChild(r);
                delete rows[k];
            }
        });

        /* Only re-append when the order really changed: moving nodes on every
         * poll drops the focus and makes the table jump. */
        var current = [];
        for (var i = 0; i < body.children.length; i++) {
            current.push(body.children[i]);
        }
        var changed = current.length !== order.length;
        for (var j = 0; !changed && j < order.length; j++) {
            if (current[j] !== order[j]) {
                changed = true;
            }
        }
        if (changed) {
            order.forEach(function (row) { body.appendChild(row); });
        }

        var empty = byId('sms-empty');
        if (empty) empty.textContent = list.length === 0 ? '收件箱为空' : '';
    }

    function scheduleNext() {
        if (timer) clearTimeout(timer);
        timer = setTimeout(load, POLL_MS);
    }

    function refreshNow() {
        if (timer) clearTimeout(timer);
        timer = null;
        load();
    }

    function load() {
        if (inFlight) {
            scheduleNext();
            return;
        }
        inFlight = true;
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
            inFlight = false;
            scheduleNext();
        });
    }

    function remove(index) {
        if (!window.confirm('确定删除这条短信吗？')) return;
        CPE.postJson(DELETE_URL, { index: index }).then(function () {
            refreshNow();
        }).catch(function (error) {
            window.alert(CPE.errorText(error));
        });
    }

    function send() {
        var num = byId('sms-number');
        var text = byId('sms-text');
        if (!num || !text) return;
        if (!num.value || !text.value) {
            setBanner('号码和内容都不能为空。', 'status-banner-warn');
            return;
        }
        var btn = byId('sms-send-btn');
        if (btn) btn.disabled = true;
        CPE.postJson(SEND_URL, { number: num.value, text: text.value }, 70000).then(function () {
            setBanner('发送成功。', 'status-banner-info');
            text.value = '';
            updateCount();
            refreshNow();
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

        var btn = byId('sms-send-btn');
        if (btn) btn.addEventListener('click', send);
        var text = byId('sms-text');
        if (text) text.addEventListener('input', updateCount);
        setFormEnabled(false);

        /* Background tabs throttle timers; refresh as soon as the page is
         * visible again so a long-paused list does not stay stale. */
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) refreshNow();
        });

        CPE.bootstrap().then(function () {
            load();
        }).catch(function () {
            /* Forced password change already redirected to the login page. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
