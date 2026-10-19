/*
 * Shared page chrome for the management console.
 *
 * The header (logo + menu button) and the left navigation used to be copied
 * into every HTML page. They are generated here instead, so adding or renaming
 * a page only touches NAV_ITEMS. Every page provides:
 *
 *   <div class="header-base" id="app-header"></div>
 *   <div class="left-base" id="leftMenu"></div>   (not on the login page)
 *
 * This script runs synchronously at the end of <body>, before the page script,
 * so menuBtn/leftMenu exist by the time the page binds its handlers.
 */
'use strict';

(function () {
    var NAV_ITEMS = [
        { href: 'index.html', text: '系统状态', pages: ['/', '/index.html'] },
        { href: 'wlan.html', text: 'WLAN设置', pages: ['/wlan.html'] },
        { href: 'upgrade.html', text: '系统升级', pages: ['/upgrade.html'] }
    ];

    function renderHeader(withMenu) {
        var header = document.getElementById('app-header');
        if (!header) {
            return;
        }
        header.className = withMenu ? 'header-base flex flex-jcb' : 'header-base';

        var logo = document.createElement('img');
        logo.className = 'header-logo';
        logo.src = './img/logoBlack.png';
        logo.alt = 'logo';
        header.appendChild(logo);

        if (withMenu) {
            var menuButton = document.createElement('div');
            menuButton.className = 'header-menu';
            menuButton.id = 'menuBtn';
            header.appendChild(menuButton);
        }
    }

    function renderNav() {
        var nav = document.getElementById('leftMenu');
        if (!nav) {
            return;
        }
        var path = window.location.pathname;
        var list = document.createElement('ul');
        NAV_ITEMS.forEach(function (item) {
            var entry = document.createElement('li');
            var link = document.createElement('a');
            link.href = item.href;
            link.textContent = item.text;
            if (item.pages.indexOf(path) !== -1) {
                link.className = 'content';
            }
            entry.appendChild(link);
            list.appendChild(entry);
        });
        nav.appendChild(list);
    }

    renderHeader(document.getElementById('leftMenu') !== null);
    renderNav();
}());
