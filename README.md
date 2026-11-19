# yrs-watch

Ubuntu GNOME 顶栏上的轻量 C++20 监控工具。点击图标查看 Codex 和 Antigravity 的各项剩余额度、重置时间，以及整机内存和 CPU。菜单使用系统主题，收起后停止系统指标采样。

## 安装

适用于 Ubuntu 26.04 / GNOME，需启用系统自带的 AppIndicators 扩展。构建需要 g++、Make、pkg-config、GLib/GIO、JSON-GLib 和 Pango 开发包。Ubuntu 上可运行：

```sh
sudo apt install g++ make pkg-config libglib2.0-dev libjson-glib-dev libpango1.0-dev
make -j
make test
make install
```

这台 Ubuntu 已有 GNOME 运行库。若希望在没有 sudo 权限的情况下重新构建，运行 `make local-deps` 即可把所需开发头文件下载到项目的 `build/` 目录，之后使用上面的 `make`、`make test` 和 `make install`。

在应用列表打开 **yrs-watch**，点击顶栏的仪表图标。以后登录时默认自动启动，可在菜单中取消勾选。更新时再次执行 `make install`，从菜单退出再打开即可。卸载时先从菜单退出，再运行 `make uninstall`。可选地删除用户缓存目录中的 `yrs-watch/quota.cache`。

## 使用

- Codex 与 Antigravity 沿用各自客户端的登录状态。分别用 `codex` 登录 ChatGPT 账号、用 `agy` 登录 Antigravity 后即可查询。监控使用两者的只读额度命令。
- 模型、周期、余量和本地重置时间分列显示，两个提供方共用相同的列位置。`week` 表示周额度，`5h` 表示五小时额度。接近 0% 或 100% 时保留小数精度，只有服务端确认用尽或全满才显示 0% 或 100%。
- Antigravity 使用 `agy -p /usage --output-format json` 获取实时额度。监控查询关闭 CLI 自更新检查，并复用同一版本的能力检查结果。平时独立打开 Antigravity 时沿用原配置。[额度查询](https://antigravity.google/docs/cli/commands/usage/) · [CLI 更新设置](https://antigravity.google/docs/cli/troubleshooting/)
- 启动后 30 秒开始查询，此后每 15 分钟更新。打开面板时，超过 5 分钟的读数会补刷。点击某一来源仅刷新该来源；「立即刷新」依次检查两者，代理刚连上时也能使用。两个官方客户端错开运行，降低查询时的内存峰值。
- 查不到时展示上次成功的读数和时间。连接失败后按 2、5、15 分钟重试。低于或等于 20%、额度耗尽、耗尽后确认恢复时会发出桌面通知。
- 内存以 GiB 显示，从总量中减去系统仍可提供的内存。CPU 显示所有核心合计的忙碌比例。首次展开约一秒开始显示 CPU，此后每三秒采样。
- 仅在本机 XDG 缓存目录保存最近额度和提醒状态。GNOME 会话中应能访问已登录的客户端；也会查找用户的 `~/.local/bin`。
