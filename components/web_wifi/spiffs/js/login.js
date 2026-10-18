/*
 * Login page for the ESP-CPE management console.
 *
 * Credentials are submitted as JSON to POST /api/login. The server answers
 * with a HttpOnly session cookie plus a CSRF token; the token is kept in
 * sessionStorage (the cookie itself is not readable from JavaScript).
 */
'use strict';

(function () {
    function byId(id) {
        return document.getElementById(id);
    }

    function setError(text) {
        var node = byId('login-error');
        if (node) {
            node.textContent = text || '';
        }
    }

    function submitLogin(event) {
        if (event) {
            event.preventDefault();
        }
        var usernameInput = byId('username');
        var passwordInput = byId('password');
        var submitButton = byId('btn-login');
        if (!usernameInput || !passwordInput) {
            return;
        }
        var username = usernameInput.value.trim();
        var password = passwordInput.value;
        if (username.length === 0 || password.length === 0) {
            setError('请输入用户名和密码');
            return;
        }
        if (submitButton) {
            submitButton.disabled = true;
        }
        setError('');
        CPE.postJson('/api/login', {
            username: username,
            password: password
        }).then(function (data) {
            CPE.setCsrf(data.csrf);
            window.location.href = '/index.html';
        }).catch(function (error) {
            if (submitButton) {
                submitButton.disabled = false;
            }
            passwordInput.value = '';
            setError(CPE.errorText(error));
        });
    }

    function init() {
        var form = byId('login-form');
        var submitButton = byId('btn-login');
        if (form) {
            form.addEventListener('submit', submitLogin);
        } else if (submitButton) {
            submitButton.addEventListener('click', submitLogin);
        }
        /* Already authenticated? Go straight to the status page. */
        CPE.getStatus().then(function () {
            window.location.href = '/index.html';
        }).catch(function () {
            /* Not logged in yet - stay on this page. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
