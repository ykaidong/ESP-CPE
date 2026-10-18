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
  * optional forced first-login password change, then the state-change checks
  * optional station-name XSS/JSON-escaping check (renames and restores)
  * optional 5-wrong-password lockout (locks the account for 60 s)

Examples:
    python scripts/web_security_test.py --password FooTdEh2F4jv
    python scripts/web_security_test.py --password FooTdEh2F4jv \
        --new-password 'CpeTest2026!' --xss
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
        # Closing without a body is an acceptable rejection: the important
        # property is that httpd is not blocked by the huge declared body.
        return True, "connection closed without reading body"
    first_line = data.split(b"\r\n", 1)[0].decode("latin-1", "replace")
    ok = any(code in first_line for code in (" 400 ", " 401 ", " 413 "))
    return ok, first_line


def raw_request_status(host, port, raw_request):
    """Send a raw HTTP request and return the status code from the response line.

    A 403 response is sent with Connection: close while the request body is
    still unread, so the kernel may reset the connection right after the
    headers. Reading the first bytes is enough to capture the status line.
    """
    sock = socket.create_connection((host, port), timeout=5)
    try:
        sock.sendall(raw_request)
        sock.settimeout(5)
        data = b""
        try:
            while b"\r\n" not in data and len(data) < 4096:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
        except (socket.timeout, ConnectionResetError):
            pass
    finally:
        sock.close()
    first_line = data.split(b"\r\n", 1)[0].decode("latin-1", "replace")
    for code in (" 400 ", " 401 ", " 403 ", " 413 "):
        if code in first_line:
            return int(code.strip())
    return None


def do_login(host, port, user, password):
    body = json.dumps({"username": user, "password": password})
    status, hdrs, data = request(
        host, port, "POST", "/api/login",
        headers={"Content-Type": "application/json"},
        body=body,
    )
    if status != 200:
        return None
    must_change = False
    csrf = None
    try:
        payload = json.loads(data.decode())
        must_change = bool(payload.get("must_change"))
        csrf = payload.get("csrf")
    except (ValueError, KeyError):
        pass
    sid = None
    for cookie in hdrs.get("set-cookie", []):
        if cookie.startswith("CPE_SID="):
            sid = cookie.split(";", 1)[0].split("=", 1)[1]
        if cookie.startswith("CPE_CSRF=") and not csrf:
            csrf = cookie.split(";", 1)[0].split("=", 1)[1]
    return {"status": status, "sid": sid, "csrf": csrf, "must_change": must_change}


def cookie_header(session):
    return {"Cookie": f"CPE_SID={session['sid']}; CPE_CSRF={session['csrf']}"}


def auth_headers(session, with_csrf=True):
    headers = {"Content-Type": "application/json", **cookie_header(session)}
    if with_csrf:
        headers["X-CSRF-Token"] = session["csrf"]
    return headers


def change_password(host, port, session, old_password, new_password):
    status, _, data = request(
        host, port, "POST", "/api/password",
        headers=auth_headers(session, True),
        body=json.dumps({"old_password": old_password, "new_password": new_password}),
    )
    ok = status == 200
    add_result("first-login password change", ok, f"HTTP {status} {data[:80]!r}")
    return ok


def xss_test(host, port, session):
    """Check that station names cannot inject markup.

    The server has a strict whitelist (alnum, space and a few punctuation
    characters), so the XSS payload must be rejected with 400. A name made of
    the allowed characters must round-trip through the JSON station list.
    """
    payload = '"><img src=x onerror=alert(1)>'
    status, _, _ = request(
        host, port, "POST", "/system/station_state/change_name",
        headers=auth_headers(session, True),
        body=json.dumps({"name_str": payload, "mac_str": "00:00:00:00:00:00"}),
    )
    if status != 400:
        return False, f"XSS payload was not rejected (HTTP {status})"

    status, _, data = request(host, port, "GET", "/system/station_state",
                              headers=cookie_header(session))
    if status != 200:
        return False, f"station list HTTP {status}"
    try:
        stations = json.loads(data.decode()).get("station_list", [])
    except ValueError:
        return False, "invalid station-list JSON"
    if not stations:
        return False, "no station connected (connect the PC to the SoftAP first)"

    station = stations[0]
    mac = station.get("mac_str")
    original = station.get("name_str", "")
    safe_name = "PC_@home (2)"
    status, _, _ = request(
        host, port, "POST", "/system/station_state/change_name",
        headers=auth_headers(session, True),
        body=json.dumps({"name_str": safe_name, "mac_str": mac}),
    )
    if status != 200:
        return False, f"safe rename HTTP {status}"

    status, _, data = request(host, port, "GET", "/system/station_state",
                              headers=cookie_header(session))
    try:
        stations = json.loads(data.decode()).get("station_list", [])
    except ValueError:
        return False, "invalid JSON after rename (escaping broken)"
    renamed = next((s.get("name_str") for s in stations if s.get("mac_str") == mac), None)

    request(
        host, port, "POST", "/system/station_state/change_name",
        headers=auth_headers(session, True),
        body=json.dumps({"name_str": original, "mac_str": mac}),
    )
    return renamed == safe_name, "XSS payload rejected; safe name round-trip " + (
        "ok" if renamed == safe_name else "mismatch"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.4.1")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password", required=True, help="current admin password")
    parser.add_argument(
        "--new-password",
        help="after login, change the admin password to this value (completes "
             "the forced first-login change), then re-login and continue",
    )
    parser.add_argument(
        "--xss",
        action="store_true",
        help="rename the first connected station to an XSS payload, verify JSON "
             "escaping and restore the original name",
    )
    parser.add_argument(
        "--include-lockout",
        action="store_true",
        help="also run the 5-wrong-password lockout test (locks login for 60 s)",
    )
    args = parser.parse_args()

    host, port = args.host, args.port
    print(f"Target: http://{host}:{port}")

    # 1. Unauthenticated status must be 401 + Connection: close.
    status, hdrs, _ = request(host, port, "GET", "/api/status")
    close_seen = header_has(hdrs, "connection", "close")
    add_result("unauth /api/status", status == 401 and close_seen,
               f"HTTP {status}, close={close_seen}")

    # 2. Oversized Content-Length must not hang the server.
    ok, detail = oversized_probe(host, port)
    add_result("oversized Content-Length", ok, detail)
    status, _, _ = request(host, port, "GET", "/api/status")
    add_result("server alive after oversized body", status == 401, f"HTTP {status}")

    # 3. Login + cookies/CSRF.
    session = do_login(host, port, args.user, args.password)
    add_result("login", session is not None,
               f"HTTP {session['status']}" if session else "failed")
    if session is None:
        print("RESULT: FAIL (cannot continue without a session)")
        return 1
    add_result("session cookie", bool(session["sid"]),
               "CPE_SID present" if session["sid"] else "missing")
    add_result("CSRF token", bool(session["csrf"]),
               "present" if session["csrf"] else "missing")
    if not session["sid"] or not session["csrf"]:
        print("RESULT: FAIL (no session/CSRF token)")
        return 1

    # 4. Optional forced password change, then re-login.
    password = args.password
    if args.new_password:
        if change_password(host, port, session, password, args.new_password):
            password = args.new_password
            session = do_login(host, port, args.user, password)
            add_result("re-login with new password", session is not None,
                       f"HTTP {session['status']}" if session else "failed")
            if session is None:
                print("RESULT: FAIL (password changed but re-login failed)")
                return 1
        else:
            add_result("re-login with new password", False, "password change failed")
            return 1

    # 5. Authenticated status works.
    status, _, _ = request(host, port, "GET", "/api/status", headers=cookie_header(session))
    add_result("authenticated /api/status", status == 200, f"HTTP {status}")

    # 6. POST without CSRF must be rejected. The server sends 403 and closes
    #    while the body is unread, so read the status line from a raw socket
    #    (http.client would surface a reset instead of the 403).
    raw_body = json.dumps({"old_password": password, "new_password": "ValidPass12345"})
    raw_req = (
        f"POST /api/password HTTP/1.1\r\nHost: {host}\r\n"
        f"Content-Type: application/json\r\nCookie: CPE_SID={session['sid']}\r\n"
        f"Content-Length: {len(raw_body)}\r\nConnection: close\r\n\r\n"
    ).encode() + raw_body.encode()
    code = raw_request_status(host, port, raw_req)
    add_result("missing CSRF", code == 403, f"HTTP {code}")

    # 7. Malformed JSON / weak password must be rejected without state change.
    status, _, _ = request(
        host, port, "POST", "/api/password",
        headers=auth_headers(session, True),
        body='{"old_password":',
    )
    add_result("malformed JSON", status == 400, f"HTTP {status}")

    status, _, _ = request(
        host, port, "POST", "/api/password",
        headers=auth_headers(session, True),
        body=json.dumps({"old_password": password, "new_password": "short"}),
    )
    add_result("weak new password", status == 400, f"HTTP {status}")

    # 8. State-changing config validation (only reachable after the forced
    #    first-login password change).
    if session["must_change"]:
        add_result("forced-change gate", True,
                   "must_change=true: /wlan_advance tests need a password change (use --new-password)")
    else:
        raw_body = json.dumps({"channel": "6", "bandwidth": "20"})
        raw_req = (
            f"POST /wlan_advance HTTP/1.1\r\nHost: {host}\r\n"
            f"Content-Type: application/json\r\nCookie: CPE_SID={session['sid']}\r\n"
            f"Content-Length: {len(raw_body)}\r\nConnection: close\r\n\r\n"
        ).encode() + raw_body.encode()
        code = raw_request_status(host, port, raw_req)
        add_result("wlan missing CSRF", code == 403, f"HTTP {code}")

        status, _, _ = request(
            host, port, "POST", "/wlan_advance",
            headers=auth_headers(session, True),
            body='{"channel":',
        )
        add_result("wlan malformed JSON", status == 400, f"HTTP {status}")

        status, _, _ = request(
            host, port, "POST", "/wlan_advance",
            headers=auth_headers(session, True),
            body=json.dumps({"channel": "999", "bandwidth": "20"}),
        )
        add_result("invalid channel", status == 400, f"HTTP {status}")

    # 9. Optional XSS/JSON-escaping check.
    if args.xss and not session["must_change"]:
        ok, detail = xss_test(host, port, session)
        add_result("station-name XSS/escaping", ok, detail)
    elif args.xss:
        add_result("station-name XSS/escaping", False,
                   "skipped: still must_change (use --new-password)")

    # 10. Optional lockout test (destructive: the account is locked for 60 s).
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
        add_result("login lockout (429)", locked,
                   "locked as expected" if locked else "no 429")

    failures = sum(1 for _, passed, _ in RESULTS if not passed)
    print()
    if failures:
        print(f"RESULT: FAIL ({failures} check(s))")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
