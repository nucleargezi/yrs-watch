"""Install or remove yrs-watch in the current user's XDG directories."""
import configparser
import os
import shutil
import sys
import tempfile
from pathlib import Path

app_id = "io.github.yorisou.YrsWatch"
home = Path.home()
binary = home / ".local/bin/yrs-watch"
font_helper = binary.with_name("yrs-watch-font-metrics")
desktop = Path(os.environ.get("XDG_DATA_HOME") or home / ".local/share") / "applications" / (app_id + ".desktop")
autostart = Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config") / "autostart" / desktop.name


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


if __name__ == "__main__":
    action = sys.argv[1]
    if action == "install":
        source = Path(sys.argv[2])
        helper = source.with_name(font_helper.name)
        if not all(path.is_file() for path in (source, helper)):
            raise SystemExit("Build yrs-watch and its font helper with make first.")
        write_atomic(font_helper, helper, 0o755)
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
    elif action == "uninstall":
        for item in (autostart, desktop, binary, font_helper):
            item.unlink(missing_ok=True)
        print("Removed the user installation. Quit the tray app if it is running.")
    else:
        raise SystemExit("usage: install.py install BINARY | uninstall")
