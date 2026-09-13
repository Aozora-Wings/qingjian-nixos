# Linux 版（fcitx5）

青简 Linux 移植：与 Windows / macOS 同构的「核心引擎独立进程 + 输入法框架薄壳」架构。

```
┌────────────────────────────┐      Unix domain socket      ┌─────────────────────────────┐
│ fcitx5 进程                │  ◄────────────────────────►  │ qingjian-server（Rust）     │
│  ┌──────────────────────┐  │    $XDG_RUNTIME_DIR/          │  ┌───────────────────────┐  │
│  │ qingjian.so（本目录） │  │    qingjian.sock             │  │ Engine（词库/学习/翻译 │  │
│  │ C++ 薄壳：键转发、     │  │    4 字节 LE 长度 + JSON     │  │ /LM/云联想/rescore）   │  │
│  │ preedit 与候选显示     │  │                              │  └───────────────────────┘  │
│  └──────────────────────┘  │                              └─────────────────────────────┘
└────────────────────────────┘
```

- `server/`：Rust Server 进程，复用 `apps/windows/server` 的 `assembly` / `dispatch` / `ipc::serve` 与
  `error`，传输层换成 Unix domain socket（`src/ipc/unix.rs`）。候选窗口不自绘——由 fcitx5 面板显示，
  所以 `dispatch` 的候选 / 状态条输出端用默认 `NoopSink`。
- `fcitx5/`：fcitx5 输入法插件（C++ 薄壳）：按键 → 协议 `KeyEvent`（fcitx5 keysym 映射为 Windows VK
  码，`dispatch/key/codes.rs` 的域），回复按 `Consumed` / `Passthrough` 决定吃掉或放行，
  `frame` 转成 fcitx5 的 preedit + 候选列表。协议字段与 Rust 侧 serde 序列化对齐（见
  `crates/qingjian-platform/src/protocol/`）。

## 构建

```bash
# Rust server（在仓库根）
nix develop   # 或 nix shell nixpkgs#rustup，然后 rustup toolchain install 1.96.0
cargo build -p qingjian-linux-server

# fcitx5 插件（本目录）
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build   # 装到 /usr/lib/fcitx5 与 /usr/share/fcitx5/addon
```

## 运行

```bash
# 1) 起 Server（systemd user 单元 / 登录自启）
./target/release/qingjian-server

# 2) fcitx5 里添加「青简」输入法
fcitx5-configtool   # 或 fcitx5 托盘 → 配置 → 添加输入法 → 青简
```

数据资源：随包 `data/generated/*.qj`（dict.qj、glossary-*.qj、english.tsv 等）与 `assets/`；
Server 按 `resources::bundled_root()` 找（装机布局与可执行文件同级，开发布局是仓库根）。
可用 `QINGJIAN_DICT` / `QINGJIAN_GLOSSARY` 环境变量覆盖。产品数据包
（`qingjian-data.tar.gz` + `model.qjm`）从上游 GitHub `data` 预发布 Release 获取。

## 已知限制（Phase 1）

- 组句期间的云联想 / 整句补全（`Poll` 定时拉取）尚未接线：插件只在按键时同步请求，无 fcitx5 定时器
  轮询 `Poll`。拼音主链（组句、候选、翻页、上屏）完整可用。
- preedit 的 `Rest` / `Corrected` 分段样式、`sentence` / `notice` 的样式区分未做（文本已拼接）。
- 键码映射覆盖常用键（字母、数字、标点、方向、翻页、功能键）；个别特殊键（小键盘等）待补。
- GNOME Wayland 下候选窗跟随 fcitx5 自身 UI 能力（kimpanel / classicui）。
