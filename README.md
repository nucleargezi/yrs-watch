# yrs-watch

Ubuntu GNOME 顶栏上的轻量 C++20 监控工具. 点击图标查看 Codex, Antigravity 和 Claude Code 的各项剩余额度, 重置时间, 以及整机内存和 CPU. 菜单使用系统主题, 收起后停止系统指标采样.

## 安装

适用于 Ubuntu 26.04 / GNOME, 需启用系统自带的 AppIndicators 扩展. 构建需要 g++, Make, pkg-config, GLib/GIO, JSON-GLib 和 Pango 开发包. Ubuntu 上可运行:

```sh
sudo apt install g++ make pkg-config libglib2.0-dev libjson-glib-dev libpango1.0-dev
make -j
make test
make install
```

这台 Ubuntu 已有 GNOME 运行库. 若希望在没有 sudo 权限的情况下重新构建, 运行 `make local-deps` 即可把所需开发头文件下载到项目的 `build/` 目录, 之后使用上面的 `make`, `make test` 和 `make install`.

在应用列表打开 **yrs-watch**, 点击顶栏的仪表图标. 以后登录时默认自动启动, 可在菜单中取消勾选. 更新时再次执行 `make install`, 从菜单退出再打开即可. 卸载时先从菜单退出, 再运行 `make uninstall` (若已接入 Claude Code statusline, 会先恢复原配置). 可选地删除用户缓存目录中的 `yrs-watch/` 目录. `yrs-watch --version` 显示版本.

## 使用

- Codex 与 Antigravity 沿用各自客户端的登录状态. 分别用 `codex` 登录 ChatGPT 账号, 用 `agy` 登录 Antigravity 后即可查询. 监控使用两者的只读额度命令
- 模型, 周期, 余量和本地重置时间分列显示, 各来源共用相同的列位置. `week` 表示周额度, `5h` 表示五小时额度. 接近 0% 或 100% 时保留小数精度, 只有服务端确认用尽或全满才显示 0% 或 100%
- Antigravity 使用 `agy -p /usage --output-format json` 获取实时额度. 监控查询关闭 CLI 自更新检查, 并复用同一版本的能力检查结果. 平时独立打开 Antigravity 时沿用原配置. [额度查询](https://antigravity.google/docs/cli/commands/usage/) · [CLI 更新设置](https://antigravity.google/docs/cli/troubleshooting/)
- 启动后 30 秒开始查询, 此后每 15 分钟更新. 打开面板时, 超过 5 分钟的读数会补刷. 系统从挂起唤醒约 5 秒后也会刷新. 点击某一来源仅刷新该来源; "立即刷新" 依次检查全部来源, 代理刚连上时也能使用. 官方客户端错开运行, 降低查询时的内存峰值
- 查不到时展示上次成功的读数和时间. 连接失败后按 2, 5, 15 分钟重试. 低于或等于 20%, 额度耗尽, 耗尽后确认恢复时会发出桌面通知; 同一额度的后续通知会替换前一条, 不会堆叠
- 内存以 GiB 显示, 从总量中减去系统仍可提供的内存. CPU 显示所有核心合计的忙碌比例. 首次展开约一秒开始显示 CPU, 此后每三秒采样
- 仅在本机 XDG 缓存目录保存最近额度和提醒状态 (权限 0600). GNOME 会话中应能访问已登录的客户端; 也会查找用户的 `~/.local/bin`

## Claude Code

Claude Code 没有只读的非交互额度命令, 但会把订阅用量 (`rate_limits`) 随 statusline 输入一起提供给 statusline 命令. `make install` 会安装 `yrs-watch-statusline`, 再运行一次:

```sh
make claude-statusline
```

即可在 `~/.claude/settings.json` (或 `$CLAUDE_CONFIG_DIR/settings.json`) 中接入. 该命令只修改 `statusLine` 一项, 修改前备份为 `settings.json.yrs-watch.bak`, 并保持文件权限 0600. 若已有自己的 statusline 命令, 会以 `--chain` 方式保留它: 原命令仍收到相同输入, 其输出照常显示. 运行 `make claude-statusline-remove` 恢复原配置.

- 之后每次使用 Claude Code, 菜单中会出现 Claude 分组; 在此之前该分组保持隐藏
- 只有 Claude.ai 订阅 (Pro / Max) 登录才提供 `rate_limits`. 使用 API key 或第三方网关时没有这项数据, Claude 分组不会出现
- 快照只保存各窗口的用量百分比和重置时间, 不保存工作目录, 会话 ID 或对话路径. 长时间未使用 Claude Code 时显示上次读数的时间, 且过期读数不会触发通知

## 菜单对齐原理

托盘菜单是 GNOME Shell AppIndicator 扩展渲染的原生文本, 无法自定义布局. yrs-watch 用制表符排列各列: 扩展使用 Pango 渲染菜单文字, Pango 会把每个 TAB 展开到下一个默认制表位. 独立的 `yrs-watch-font-metrics` 进程按桌面字体和缩放比例测量每段文字占用的制表位, 主进程据此补足 TAB 数量. 测量完成前或测量失败时退化为单个 TAB, 内容仍可读, 只是列可能不齐. 若将来扩展改变了标签渲染方式, 对齐会受影响.

## 添加新来源

1. 在 [provider_registry.hpp](src/provider_registry.hpp) 的 `Provider` 枚举末尾加一项, 并在 `provider_table` 中按相同顺序加一行: 缓存键, 菜单标题, 类型 (`Cli` 拉起官方客户端, 同一时刻只运行一个; `Snapshot` 读取其他工具写入的文件), 以及无数据时是否隐藏
2. 新建 `src/provider_<name>.cpp` 实现 `QuotaSample fetch_<name>(std::stop_token)`, 在 [provider_impl.hpp](src/provider_impl.hpp) 中声明, 在 [providers.cpp](src/providers.cpp) 的 `switch` 中分派 (编译器会提示遗漏的分支); `Snapshot` 来源还需在 `snapshot_file()` 中返回文件路径. 命令行来源可复用 [child_process.hpp](src/child_process.hpp) 的 `Child` 与 `find_cli`
3. 把文件加入 Makefile 的 `provider_sources`, 在 `tests/provider_test.cpp` 中用模拟脚本或快照覆盖成功, 未登录与格式异常等情况

原则: 只使用官方客户端的只读命令, 或用户主动接入的快照, 不直接读取凭据文件或调用私有接口.
