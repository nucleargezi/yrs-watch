"""Install or remove yrs-watch in the current user's XDG directories."""
import configparser
import json
import os
import shlex
import shutil
import sys
import tempfile
from pathlib import Path

app_id = "io.github.yorisou.YrsWatch"
home = Path.home()
binary = home / ".local/bin/yrs-watch"
font_helper = binary.with_name("yrs-watch-font-metrics")
statusline = binary.with_name("yrs-watch-statusline")
desktop = Path(os.environ.get("XDG_DATA_HOME") or home / ".local/share") / "applications" / (app_id + ".desktop")
autostart = Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config") / "autostart" / desktop.name
claude_settings = Path(os.environ.get("CLAUDE_CONFIG_DIR") or home / ".claude") / "settings.json"
claude_backup = claude_settings.with_name("settings.json.yrs-watch.bak")


def write_atomic(path, contents, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        try:
            if isinstance(contents, Path):
                with contents.open("rb") as src:
                    shutil.copyfileobj(src, output)
            else:
                output.write(contents.encode())
            output.flush()
            os.fchmod(output.fileno(), mode)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


def launcher():
    # First quote Exec's argument, then escape the backslashes for the key file.
    argument = "".join(("\\" if c in '\\"$`' else "") + ("%%" if c == "%" else c) for c in str(binary))
    quoted = '"' + argument.replace("\\", "\\\\") + '"'
    return f"""[Desktop Entry]
Type=Application
Name=yrs-watch
Comment=AI quotas and system usage in the top bar
Comment[zh_CN]=在顶栏查看 AI 额度和系统用量
Exec={quoted}
Icon=power-profile-performance-symbolic
Terminal=false
Categories=System;Monitor;
StartupNotify=false
"""


def load_claude_settings():
    if not claude_settings.exists():
        return {}
    try:
        data = json.loads(claude_settings.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise SystemExit(f"{claude_settings} is not plain JSON ({error}); edit statusLine by hand.")
    if not isinstance(data, dict):
        raise SystemExit(f"{claude_settings} does not contain a JSON object.")
    return data


def save_claude_settings(data):
    # The file may hold credentials: keep it private.
    write_atomic(claude_settings, json.dumps(data, indent=2, ensure_ascii=False) + "\n", 0o600)


def hooked(data):
    line = data.get("statusLine")
    return isinstance(line, dict) and str(statusline) in str(line.get("command", ""))


def claude_hook():
    if not statusline.is_file():
        raise SystemExit("Run make install first.")
    data = load_claude_settings()
    if hooked(data):
        print("The Claude Code statusline already feeds yrs-watch.")
        return
    previous = data.get("statusLine")
    if previous is None:
        line = {"type": "command", "command": shlex.quote(str(statusline))}
    elif isinstance(previous, dict) and previous.get("type") == "command" and previous.get("command"):
        # Keep the user's statusline: run it behind ours with the same input.
        line = dict(previous)
        line["command"] = f"{shlex.quote(str(statusline))} --chain {shlex.quote(previous['command'])}"
    else:
        raise SystemExit("The current statusLine is not a command; add yrs-watch-statusline by hand.")
    write_atomic(claude_backup, json.dumps({"statusLine": previous}, ensure_ascii=False) + "\n", 0o600)
    data["statusLine"] = line
    save_claude_settings(data)
    print(f"Claude Code statusline now feeds yrs-watch ({claude_settings}).")


def claude_unhook():
    data = load_claude_settings()
    if not hooked(data):
        print("The Claude Code statusline does not use yrs-watch.")
        return
    previous = None
    if claude_backup.exists():
        previous = json.loads(claude_backup.read_text(encoding="utf-8")).get("statusLine")
    if previous is None:
        data.pop("statusLine", None)
    else:
        data["statusLine"] = previous
    save_claude_settings(data)
    claude_backup.unlink(missing_ok=True)
    print("Restored the previous Claude Code statusline.")


if __name__ == "__main__":
    action = sys.argv[1]
    if action == "install":
        source = Path(sys.argv[2])
        helper = source.with_name(font_helper.name)
        status_helper = source.with_name(statusline.name)
        if not all(path.is_file() for path in (source, helper, status_helper)):
            raise SystemExit("Build yrs-watch and its helpers with make first.")
        write_atomic(font_helper, helper, 0o755)
        write_atomic(statusline, status_helper, 0o755)
        write_atomic(binary, source, 0o755)
        write_atomic(desktop, launcher(), 0o644)
        # Keep the choice made in the menu when installing a newer executable.
        enabled = True
        if autostart.exists():
            previous = configparser.ConfigParser(interpolation=None, strict=False)
            previous.read(autostart, encoding="utf-8")
            enabled = not previous.getboolean("Desktop Entry", "Hidden", fallback=False) and previous.getboolean(
                "Desktop Entry", "X-GNOME-Autostart-enabled", fallback=True)
        write_atomic(autostart, launcher() + ("" if enabled else "Hidden=true\n"), 0o600)
        print(f"Installed {binary}")
    elif action == "claude-hook":
        claude_hook()
    elif action == "claude-unhook":
        claude_unhook()
    elif action == "uninstall":
        # A statusline pointing at a removed binary would break Claude Code's status bar.
        if hooked(load_claude_settings()):
            claude_unhook()
        for item in (autostart, desktop, binary, font_helper, statusline):
            item.unlink(missing_ok=True)
        print("Removed the user installation. Quit the tray app if it is running.")
    else:
        raise SystemExit("usage: install.py install BINARY | uninstall | claude-hook | claude-unhook")
