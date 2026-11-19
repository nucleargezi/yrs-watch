"""Exercise yrs-watch-statusline and the Claude Code statusline installer in a temporary home."""
import json
import os
import stat
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INPUT = {
    "model": {"display_name": "Opus"},
    "cwd": "/secret/project",
    "session_id": "secret-session",
    "transcript_path": "/secret/transcript.jsonl",
    "rate_limits": {
        "five_hour": {"used_percentage": 26, "resets_at": 1900010000},
        "seven_day": {"used_percentage": 2.5, "resets_at": 1900500000},
    },
}


def run(binary, env, payload, *args):
    text = payload if isinstance(payload, str) else json.dumps(payload)
    return subprocess.run([str(binary), *args], input=text, capture_output=True, text=True, env=env, timeout=5)


def identity(path):
    info = path.stat()
    return info.st_ino, info.st_mtime_ns


def test_helper(binary, root):
    env = os.environ | {"XDG_CACHE_HOME": str(root / "cache")}
    snapshot = root / "cache" / "yrs-watch" / "claude-statusline.json"

    out = run(binary, env, INPUT)
    assert out.returncode == 0 and out.stdout == "Opus · 5h 74% · 7d 98%\n", out
    text = snapshot.read_text()
    data = json.loads(text)
    assert set(data) == {"version", "observed_at", "windows"} and data["version"] == 1
    assert data["windows"] == {"five_hour": {"used_percentage": 26, "resets_at": 1900010000},
                               "seven_day": {"used_percentage": 2.5, "resets_at": 1900500000}}
    assert "secret" not in text, "only usage windows may be stored"
    assert stat.S_IMODE(snapshot.stat().st_mode) == 0o600
    assert stat.S_IMODE(snapshot.parent.stat().st_mode) == 0o700

    # Identical values within a minute leave the file alone; new values replace it.
    before = identity(snapshot)
    assert run(binary, env, INPUT).returncode == 0 and identity(snapshot) == before
    changed = json.loads(json.dumps(INPUT))
    changed["rate_limits"]["five_hour"]["used_percentage"] = 40
    assert run(binary, env, changed).stdout == "Opus · 5h 60% · 7d 98%\n"
    assert json.loads(snapshot.read_text())["windows"]["five_hour"]["used_percentage"] == 40

    # Logins without subscription windows record nothing.
    quiet = os.environ | {"XDG_CACHE_HOME": str(root / "quiet")}
    out = run(binary, quiet, {"model": {"display_name": "Opus"}, "rate_limits": None})
    assert out.returncode == 0 and out.stdout == "Opus\n"
    assert not (root / "quiet" / "yrs-watch" / "claude-statusline.json").exists()
    assert run(binary, quiet, "not json").stdout == "\n"

    # --chain hands the untouched input to the previous command and keeps its exit status.
    out = run(binary, env, INPUT, "--chain", "cat; exit 3")
    assert out.returncode == 3 and json.loads(out.stdout) == INPUT
    assert run(binary, env, INPUT, "--bogus").returncode == 2


def test_installer(root):
    home = root / "home"
    helper = home / ".local/bin/yrs-watch-statusline"
    helper.parent.mkdir(parents=True)
    helper.write_text("")
    env = {k: v for k, v in os.environ.items() if not k.startswith("XDG_") and k != "CLAUDE_CONFIG_DIR"}
    env["HOME"] = str(home)
    settings = home / ".claude" / "settings.json"
    backup = settings.with_name("settings.json.yrs-watch.bak")

    def tool(action, check=True):
        return subprocess.run([sys.executable, str(ROOT / "tools" / "install.py"), action],
                              env=env, capture_output=True, text=True, check=check)

    def load():
        return json.loads(settings.read_text())

    # No previous statusline.
    tool("claude-hook")
    assert load()["statusLine"] == {"type": "command", "command": str(helper)}
    assert stat.S_IMODE(settings.stat().st_mode) == 0o600
    tool("claude-hook")
    assert load()["statusLine"]["command"] == str(helper), "installing twice changes nothing"
    tool("claude-unhook")
    assert "statusLine" not in load() and not backup.exists()

    # A previous command statusline is chained, and restored exactly.
    original = {"type": "command", "command": "bash ~/status.sh", "padding": 0}
    settings.write_text(json.dumps({"env": {"KEEP": "1"}, "statusLine": original}))
    tool("claude-hook")
    data = load()
    assert data["statusLine"] == {"type": "command", "command": f"{helper} --chain 'bash ~/status.sh'", "padding": 0}
    assert data["env"] == {"KEEP": "1"}
    tool("claude-unhook")
    assert load() == {"env": {"KEEP": "1"}, "statusLine": original}

    # Uninstalling restores the statusline before the helper disappears.
    tool("claude-hook")
    tool("uninstall")
    assert load()["statusLine"] == original and not helper.exists()

    # Anything but a command statusline is left for the user to edit.
    settings.write_text(json.dumps({"statusLine": "plain"}))
    helper.write_text("")
    assert tool("claude-hook", check=False).returncode != 0
    assert load() == {"statusLine": "plain"}


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as temp:
        test_helper(Path(sys.argv[1]).resolve(), Path(temp))
        test_installer(Path(temp))
    print("statusline tests passed")
