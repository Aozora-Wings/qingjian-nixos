# qingjian-nixos

青简输入法（[qingjian-team/qingjian](https://github.com/qingjian-team/qingjian)）的 **NixOS 集成框架**（Linux 版）。

## 定位

本仓库**不再 fork 官方整树**，只维护 Linux 移植所需的两块：

- `apps/linux/`：Linux 专属源码 —— Rust `server`（引擎进程，Unix socket IPC）与 `fcitx5` 插件（C++）
- `modules/` + `packages/`：NixOS 模块与 Nix 打包（合并官方 crates 构建 `qingjian-server` / `qingjian.so`）

官方源码（crates/apps-windows-macos/tools）由 **flake input `qingjian`** 静态拉取，构建时与本地 `apps/linux` 合并：

```nix
# flake.nix inputs
qingjian = {
  url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/archive/refs/heads/main.tar.gz";
  flake = false;
};
```

- 官方更新 → `nix flake lock --update-input qingjian` 跟进，无需 rebase
- 官方 crates API 变动导致构建失败 → 适配 `apps/linux/server/src` 后重新构建
- 数据包（`qingjian-data` / `qingjian-model`）由官方 `tools/release/data.lock` 控制版本（tag + sha256），也作 flake input 引用

## 用法

### 作为 NixOS 模块引入

在 `nixos-config` 的 flake inputs 里引用本仓库：

```nix
qingjian-nixos = {
  url = "git+https://ghfast.top/https://github.com/Aozora-Wings/qingjian-nixos?ref=main";
  inputs.nixpkgs.follows = "nixpkgs";  # 跟随主配置 nixpkgs，避免双份
};
```

桌面环境共用模块里启用：

```nix
# fcitx5 输入法（系统级模块）
{ config, pkgs, lib, inputs, ... }:
{
  i18n.inputMethod = {
    type = "fcitx5";
    enable = true;
    fcitx5.waylandFrontend = true;
    fcitx5.addons = [
      inputs.qingjian-nixos.nixosModules.default
    ];
  };
  environment.sessionVariables = {
    GTK_IM_MODULE = "fcitx";
    QT_IM_MODULE = "fcitx";
    XMODIFIERS = "@im=fcitx";
  };
}
```

> 数据包体积大，建议在 nixos-config 里以二进制包形式单独引用（`flake = false` + `url` 指向 GitHub release），而不是打进模块默认包。具体见 `modules/nixos.nix` 的 `services.qingjian.dataDir` 注入方式。

### 本地开发

```bash
# 进入开发环境（含 rust 工具链与构建依赖）
nix develop

# 生成/更新完整 Cargo.lock（官方或 nixpkgs 更新后）
nix run .#gen-lock -- packages/linux-workspace.lock

# 构建 server（合并官方源码）
nix build .#qingjianServer
```

### CI

- `check-upstream.yml`：每天 02:00 UTC 检查官方更新 → 拉新 lock → 构建验证（cargoHash 过期自动修复；官方 API 变动开 issue 通知）
- `update-data.yml`：每两天检查官方 `data.lock` → 数据 tag/hash 变化时更新数据 input 并推送

## 目录

```
apps/linux/server      Rust server（qingjian-linux-server crate）
apps/linux/fcitx5      fcitx5 插件（C++，加载时连接 server）
modules/nixos.nix      NixOS 模块（fcitx5 addon + systemd user 服务）
packages/server.nix    server 打包（合并官方源码 + linux-workspace.lock）
packages/fcitx5.nix    插件打包
packages/linux-workspace.lock  完整 Cargo.lock（含 apps/linux/server 依赖）
```

## 非 Nix 发行版

青简官方未发布 Linux 版；本仓库的 `apps/linux` 源码可独立编译（`server` + `fcitx5` 插件），欢迎按发行版打包脚本分发。NixOS 用户直接用本 flake 即可。

## License

GPL-3.0-or-later（跟随官方）。`apps/linux` 为对本仓库贡献的代码。
