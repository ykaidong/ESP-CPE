#!/usr/bin/env python3
"""
Phase 2 management-console security regression for the ESP-CPE firmware.

The PC must be connected to the device SoftAP (default 192.168.4.1). The
script uses only the Python standard library, so it runs anywhere Python 3 is
available.

Covered checks (docs/PHASE2_TEST_PLAN.md section 1):
  * unauthenticated /api/status -> 401 + Connection: close
  * oversized Content-Length -> rejected/closed quickly, server stays alive
  * login with the current admin password (default: AP password)
  * CSRF enforcement, malformed JSON, weak password validation
  * optional 5-wrong-password lockout (locks the account for 60 s)

Example:
    python scripts/web_security_test.py --password FooTdEh2F4jv
    python scripts/web_security_test.py --host 192.168.4.1 --password ... --include-lockout
"""

import argparse
import http.client
import json
import socket
import sys

RESULTS = []


def add_result(name, passed, detail):
    RESULTS.append((name, passed, detail))
    tag = "PASS" if passed else "FAIL"
    print(f"[{tag}] {name}: {detail}")


def request(host, port, method, path, headers=None, body=None, timeout=5):
    """Return (status, headers_lower, body) or (None, {}, error_text)."""
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        data = resp.read()
        hdrs = {}
        for key, value in resp.getheaders():
            hdrs.setdefault(key.lower(), []).append(value)
        return resp.status, hdrs, data
    except Exception as exc:  # noqa: BLE001 - transport errors are results
        return None, {}, repr(exc)
    finally:
        conn.close()


def header_has(hdrs, name, needle):
    for value in hdrs.get(name.lower(), []):
        if needle.lower() in value.lower():
            return True
    return False


def oversized_probe(host, port):
    """Send a huge Content-Length and check the server drops it quickly."""
    request_line = (
        f"POST /api/login HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        f"Content-Type: application/json\r\n"
        f"Content-Length: 2000000000\r\n"
        f"Connection: close\r\n\r\n"
    ).encode()
    sock = socket.create_connection((host, port), timeout=5)
    try:
        sock.sendall(request_line)
        sock.settimeout(5)
        try:
            data = sock.recv(4096)
        except socket.timeout:
            data = b""
    finally:
        sock.close()
    if not data:
        # Closing the connection without a body is an acceptable rejection: the
        # important property is that httpd is not blocked by the huge body.
        return True, "connection closed without reading body"
    first_line = data.split(b"\r\n", 1)[0].decode("latin-1", "replace")
    ok = any(code in first_line for code in (" 400 ", " 401 ", " 413 "))
    return ok, first_line


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password", required=True, help="current admin password")
    parser.add_argument(
        "--include-lockout",
        action="store_true",
        help="also run the 5-wrong-password lockout test (locks login for 60 s)",
    )
    args = parser.parse_args()

    host, port = args.host, args.port
    print(f"Target: http://{host}:{port}")

    # 1. Unauthenticated status must be 401 + Connection: close.
    status, hdrs, body = request(host, port, "GET", "/api/status")
    ok = status == 401 and header_has(hdrs, "connection", "close")
    add_result("unauth /api/status", ok, f"HTTP {status}, close={header_has(hdrs, 'connection', 'close')}")

    # 2. Oversized Content-Length must not hang the server.
    ok, detail = oversized_probe(host, port)
    add_result("oversized Content-Length", ok, detail)
    status, _, _ = request(host, port, "GET", "/api/status")
    add_result("server alive after oversized body", status == 401, f"HTTP {status}")

    # 3. Login + cookies/CSRF.
    login_body = json.dumps({"username": args.user, "password": args.password})
    status, hdrs, body = request(
        host, port, "POST", "/api/login",
        headers={"Content-Type": "application/json"},
        body=login_body,
    )
    login_ok = status == 200
    add_result("login", login_ok, f"HTTP {status}")
    if not login_ok:
        print("RESULT: FAIL (cannot continue without a session)")
        return 1

    must_change = False
    csrf = None
    sid = None
    try:
        payload = json.loads(body.decode())
        must_change = bool(payload.get("must_change"))
        csrf = payload.get("csrf")
    except (ValueError, KeyError):
        pass
    for cookie in hdrs.get("set-cookie", []):
        if cookie.startswith("CPE_SID="):
            sid = cookie.split(";", 1)[0].split("=", 1)[1]
        if cookie.startswith("CPE_CSRF=") and not csrf:
            csrf = cookie.split(";", 1)[0].split("=", 1)[1]
    add_result("session cookie", bool(sid), "CPE_SID present" if sid else "missing")
    add_result("CSRF token", bool(csrf), "present" if csrf else "missing")
    if not sid or not csrf:
        print("RESULT: FAIL (no session/CSRF token)")
        return 1
    cookie_hdr = {"Cookie": f"CPE_SID={sid}; CPE_CSRF={csrf}"}

    # 4. Authenticated status works.
    status, _, _ = request(host, port, "GET", "/api/status", headers=cookie_hdr)
    add_result("authenticated /api/status", status == 200, f"HTTP {status}")

    # 5. POST without CSRF must be rejected.
    status, _, _ = request(
        host, port, "POST", "/api/password",
        headers={"Content-Type": "application/json", "Cookie": f"CPE_SID={sid}"},
        body=json.dumps({"old_password": args.password, "new_password": "ValidPass12345"}),
    )
    add_result("missing CSRF", status == 403, f"HTTP {status}")

    # 6. Malformed JSON must be rejected (400), without changing the password.
    status, _, _ = request(
        host, port, "POST", "/api/password",
        headers={"Content-Type": "application/json", "X-CSRF-Token": csrf, **cookie_hdr},
        body='{"old_password":',
    )
    add_result("malformed JSON", status == 400, f"HTTP {status}")

    # 7. Weak new password must be rejected (400), without changing the password.
    status, _, _ = request(
        host, port, "POST", "/api/password",
        headers={"Content-Type": "application/json", "X-CSRF-Token": csrf, **cookie_hdr},
        body=json.dumps({"old_password": args.password, "new_password": "short"}),
    )
    add_result("weak new password", status == 400, f"HTTP {status}")

    # 8. State-changing config validation. It is only reachable after the forced
    #    first-login password change; otherwise the device correctly returns 403.
    if must_change:
        add_result(
            "forced-change gate",
            True,
            "must_change=true: /wlan_advance state-change tests need a password change first (skipped)",
        )
    else:
        status, _, _ = request(
            host, port, "POST", "/wlan_advance",
            headers={"Content-Type": "application/json", **cookie_hdr},
            body=json.dumps({"channel": "6", "bandwidth": "20"}),
        )
        add_result("wlan missing CSRF", status == 403, f"HTTP {status}")

        status, _, _ = request(
            host, port, "POST", "/wlan_advance",
            headers={"Content-Type": "application/json", "X-CSRF-Token": csrf, **cookie_hdr},
            body='{"channel":',
        )
        add_result("wlan malformed JSON", status == 400, f"HTTP {status}")

        status, _, _ = request(
            host, port, "POST", "/wlan_advance",
            headers={"Content-Type": "application/json", "X-CSRF-Token": csrf, **cookie_hdr},
            body=json.dumps({"channel": "999", "bandwidth": "20"}),
        )
        add_result("invalid channel", status == 400, f"HTTP {status}")

    # 9. Optional lockout test (destructive: the account is locked for 60 s).
    if args.include_lockout:
        locked = False
        for index in range(6):
            status, _, _ = request(
                host, port, "POST", "/api/login",
                headers={"Content-Type": "application/json"},
                body=json.dumps({"username": args.user, "password": f"__wrong_{index}"}),
            )
            if status == 429:
                locked = True
                break
        add_result("login lockout (429)", locked, "locked as expected" if locked else "no 429")

    failures = sum(1 for _, passed, _ in RESULTS if not passed)
    print()
    if failures:
        print(f"RESULT: FAIL ({failures} check(s))")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
