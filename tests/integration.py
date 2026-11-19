"""Exercise the installed desktop protocols on an isolated bus and synthetic accounts."""
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

APP = "io.github.yorisou.YrsWatch"
MENU = "com.canonical.dbusmenu."
NOTIFY_MOCK = r'''import json, sys
from pathlib import Path
from gi.repository import Gio, GLib

log_path = Path(sys.argv[1])
XML = """<node>
  <interface name="org.freedesktop.Notifications">
    <method name="Notify">
      <arg type="s" name="app_name" direction="in"/>
      <arg type="u" name="replaces_id" direction="in"/>
      <arg type="s" name="app_icon" direction="in"/>
      <arg type="s" name="summary" direction="in"/>
      <arg type="s" name="body" direction="in"/>
      <arg type="as" name="actions" direction="in"/>
      <arg type="a{sv}" name="hints" direction="in"/>
      <arg type="i" name="expire_timeout" direction="in"/>
      <arg type="u" name="id" direction="out"/>
    </method>
  </interface>
</node>"""
iface = Gio.DBusNodeInfo.new_for_xml(XML).lookup_interface("org.freedesktop.Notifications")
count = 0

def on_call(conn, sender, path, iface_name, method, params, inv):
    global count
    if method == "Notify":
        _, _, app_icon, summary, body, _, _, _ = params.unpack()
        count += 1
        with log_path.open("a", encoding="utf-8") as out:
            out.write(json.dumps({"icon": app_icon, "summary": summary, "body": body}, ensure_ascii=False) + "\n")
        inv.return_value(GLib.Variant("(u)", (count,)))

def on_bus(conn, name):
    reg = getattr(conn, "register_object_with_closures2", conn.register_object)
    reg("/org/freedesktop/Notifications", iface, on_call, None, None)

Gio.bus_own_name(Gio.BusType.SESSION, "org.freedesktop.Notifications",
    Gio.BusNameOwnerFlags.NONE, on_bus, None, None)
GLib.MainLoop().run()
'''
MOCK = '''#!/usr/bin/python3
import json, os, sys, time
from pathlib import Path
root = Path(os.environ["MOCK_STATE"])
args = sys.argv[1:]
tool = Path(sys.argv[0]).name
if tool == "agy" and args == ["--version"]:
    print("1.5.9")
    raise SystemExit()
with (root / (tool + ".calls")).open("a") as log:
    print(os.getpid(), file=log)
time.sleep(.10)
mode = (root / tool).read_text()
def send(x):
    print(json.dumps(x), flush=True)
if tool == "codex":
    assert args == ["app-server", "--listen", "stdio://"]
    init = json.loads(input())
    send({"id": init["id"], "result": {}})
    assert json.loads(input())["method"] == "initialized"
    req = json.loads(input())
    assert req["method"] == "account/rateLimits/read"
    if mode == "offline":
        send({"id": req["id"], "error": {"message": "network unavailable"}})
    else:
        send({"id": req["id"], "result": {"rateLimitsByLimitId": {"codex": {
            "primary": {"usedPercent": int(mode), "windowDurationMins": 300, "resetsAt": 1980000000}}}}})
else:
    assert args[-2:] == ["-p", "/usage"] and "json" in args
    if mode == "auth":
        send({"status":"ERROR","error":"authentication required"})
    else:
        if mode == "many":
            groups = [{"name":f"Model-{i}","buckets":[{"id":f"group-{i}","window":"weekly",
                       "remaining_fraction":1.0}]} for i in range(9)]
        else:
            groups = [{"name":"Gemini","buckets":[{"id":"group-1","window":"weekly",
                      "remaining_fraction":float(mode),"reset_time":"2032-09-29T12:00:00Z"}]}]
        send({"status":"SUCCESS","command":{"data":{"groups":groups}}})
'''


def bus(method, *args, path="/MenuBar", dest=APP):
    return subprocess.check_output(
        ["gdbus", "call", "--session", "--dest", dest, "--object-path", path,
         "--method", method, *map(str, args)], text=True, stderr=subprocess.PIPE, timeout=5)


def layout():
    return bus(MENU + "GetLayout", 0, 1, "['label','toggle-state']")


def event(item, action="clicked"):
    bus(MENU + "Event", item, action, "<int32 0>", 0)


def until(predicate, duration=6):
    end = time.monotonic() + duration
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("timed out waiting for GUI state")


def has(*text):
    state = layout()
    return all(x in state for x in text)


def run(binary, root):
    cli = root / "bin"
    cli.mkdir()
    for name, value in (("codex", "offline"), ("agy", "auth")):
        (root / name).write_text(value)
        (cli / name).write_text(MOCK)
        (cli / name).chmod(0o755)
    os.environ["PATH"] = str(cli) + os.pathsep + os.environ["PATH"]
    os.environ["MOCK_STATE"] = str(root)

    def start():
        p = subprocess.Popen([binary], stderr=subprocess.PIPE, text=True)
        subprocess.run(["gdbus", "wait", "--session", "--timeout", "5", APP], check=True)
        # The application acquires its bus name before the tray has exported the menu.
        def menu_ready():
            if p.poll() is not None:
                raise AssertionError(p.stderr.read())
            try:
                return has("立即刷新", "退出")
            except subprocess.CalledProcessError as exc:
                if "UnknownMethod" in exc.stderr:
                    return False
                raise
        until(menu_ready)
        return p

    notif_log = root / "notifications.jsonl"
    notif_proc = subprocess.Popen([sys.executable, "-c", NOTIFY_MOCK, str(notif_log)])
    subprocess.run(["gdbus", "wait", "--session", "--timeout", "5", "org.freedesktop.Notifications"], check=True)

    def read_notifs():
        if not notif_log.exists():
            return []
        return [json.loads(line) for line in notif_log.read_text(encoding="utf-8").splitlines() if line]

    p = start()
    try:
        assert has("Codex", "Antigravity", "等待连接", "CPU　—")
        subprocess.run([binary], check=True, timeout=3)  # second launch is a single instance
        bus(MENU + "AboutToShow", 0)
        time.sleep(.15)
        assert not (root / "codex.calls").exists()  # host prefetch does not trigger sampling
        event(0, "opened")
        until(lambda: has("需要登录", "等待连接", "GiB") and "CPU　—" not in layout())
        event(0, "closed")
        (root / "codex").write_text("81")
        (root / "agy").write_text(".65")
        event(1)
        until(lambda: has("19%", "65%", "更新于", "week", "5h"))
        assert all(old not in layout() for old in ("剩 ", "每周", "5 小时"))
        assert "NeedsAttention" in bus("org.freedesktop.DBus.Properties.Get",
            "org.kde.StatusNotifierItem", "Status", path="/StatusNotifierItem")
        until(lambda: len(read_notifs()) >= 1)
        notifs = read_notifs()
        assert notifs[0]["summary"] == "Codex" and notifs[0]["icon"] == "dialog-warning-symbolic"
        assert "20%" in notifs[0]["body"]

        # Exhausting and recovering Antigravity while Codex remains low must use distinct notification icons.
        (root / "agy").write_text("0")
        event(102)
        until(lambda: has("0%") and "刷新中" not in layout() and len(read_notifs()) >= 2)
        notifs = read_notifs()
        assert notifs[1]["summary"] == "Antigravity" and notifs[1]["icon"] == "dialog-warning-symbolic"
        assert "已用尽" in notifs[1]["body"]

        (root / "agy").write_text(".65")
        event(102)
        until(lambda: has("65%") and "刷新中" not in layout() and len(read_notifs()) >= 3)
        notifs = read_notifs()
        assert notifs[2]["summary"] == "Antigravity" and notifs[2]["icon"] == "power-profile-performance-symbolic"
        assert "已恢复" in notifs[2]["body"]

        # A failed query preserves the last successful measurement.
        (root / "codex").write_text("offline")
        event(1)
        until(lambda: has("19%", "上次更新", "等待连接"))
        (root / "codex").write_text("5")
        event(1)
        until(lambda: has("95%", "65%") and "刷新中" not in layout())

        for _ in range(10):
            event(1)
            until(lambda: "刷新中" not in layout())
        for file in root.glob("*.calls"):
            pids = file.read_text().splitlines()
            assert len(pids) >= 10
            assert all(not Path("/proc", pid).exists() for pid in pids)

        # A click on system metrics does not launch a client. Each quota block
        # refreshes its own provider, and paged groups expose every window.
        ag_calls = (root / "agy.calls").read_text()
        codex_calls = (root / "codex.calls").read_text()
        event(104)
        assert (root / "agy.calls").read_text() == ag_calls and (root / "codex.calls").read_text() == codex_calls
        # Formatting must keep nearly full and nearly empty windows distinct from the endpoints.
        for fraction, visible in ((".99999", ">99.9%"), (".00001", "<0.1%"), (".999", "99.9%")):
            (root / "agy").write_text(fraction)
            event(102)
            until(lambda: has(visible) and "刷新中" not in layout())
        (root / "agy").write_text("many")
        event(102)
        until(lambda: has("Model-0", "Model-3") and "刷新中" not in layout())
        assert "Model-4" not in layout() and (root / "codex.calls").read_text() == codex_calls
        event(4)
        assert has("Model-4", "Model-7")
        event(4)
        assert has("Model-8")
        event(4)
        assert has("Model-0")
        (root / "agy").write_text(".65")
        event(102)
        until(lambda: has("65%") and "刷新中" not in layout())

        event(2)
        auto = root / "config" / "autostart" / (APP + ".desktop")
        auto_text = auto.read_text()
        assert auto.exists() and "Hidden=false" in auto_text
        assert "Icon=power-profile-performance-symbolic" in auto_text and "StartupNotify=false" in auto_text
        event(2)
        assert "Hidden=true" in auto.read_text()
        event(3)
        assert p.wait(timeout=3) == 0
        errors = p.stderr.read()
        assert "CRITICAL" not in errors and "WARNING" not in errors, errors
        p = start()
        assert has("95%", "65%")
        event(3)
        assert p.wait(timeout=3) == 0
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait(timeout=3)
        if notif_proc.poll() is None:
            notif_proc.terminate()
            notif_proc.wait(timeout=3)


if __name__ == "__main__":
    if len(sys.argv) > 2:
        run(sys.argv[1], Path(sys.argv[2]))
    else:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            env = os.environ | {"XDG_CACHE_HOME": str(root / "cache"), "XDG_CONFIG_HOME": str(root / "config"),
                                "XDG_DATA_HOME": str(root / "data"), "GSETTINGS_BACKEND": "memory"}
            subprocess.run(["dbus-run-session", "--", sys.executable, __file__,
                            str(Path(sys.argv[1]).resolve()), temp], env=env, check=True, timeout=55)
        print("Desktop integration passed")
