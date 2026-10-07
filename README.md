# qingjian-nixos

青简输入法（[qingjian-team/qingjian](https://github.com/qingjian-team/qingjian)）的 Linux/NixOS 移植与整合仓库：
Rust server（官方源码） + fcitx5 插件 + NixOS / home-manager 模块。

## 定位

- 官方目前只发布 macOS/Windows 版；本仓库提供 Linux 侧引导：`apps/linux/server`
  使用官方源码（`inputs.qingjian` 锁定 rev），`apps/linux/fcitx5` 为本地维护的
  fcitx5 插件（连接 server 取词）。
- 数据包（词库/整句模型）直接引官方 GitHub Release 资产，由 CI 自动跟随
  `data.lock` 的 tag 同步，flake.lock 锁定 narHash，可溯源、可复现。

## flake.nix inputs

| input | 来源 | 说明 |
|---|---|---|
| `nixpkgs` | 南大镜像 | 跟随主配置（nixos-config 里 `follows`） |
| `qingjian` | 官方 main（ghfast 镜像） | 官方源码（flakeless），rev 由 flake.lock 锁定 |
| `qingjian-data` | 官方 Release 资产 | data-v3 起为完整资源包（`data/generated` + `data/models/hanzhang-*` + `assets`） |

## 用法

### 作为 NixOS 模块引入

在 `nixos-config` 的 flake inputs 里引用本仓库：

```nix
qingjian-nixos = {
  url = "git+https://ghfast.top/https://github.com/Aozora-Wings/qingjian-nixos?ref=main";
  inputs.nixpkgs.follows = "nixpkgs";  # 跟随主配置 nixpkgs，避免双份
};
```

桌面环境共用模块里**导入模块**（注意：`fcitx5.addons` 要的是包，不是模块——插件包由模块内部自动加入，不用也不能把 `nixosModules.default` 塞进 `addons`）：

```nix
# fcitx5 输入法（系统级模块）
{ config, pkgs, lib, inputs, ... }:
{
  imports = [ inputs.qingjian-nixos.nixosModules.default ];
  services.qingjian.enable = true;

  i18n.inputMethod = {
    type = "fcitx5";
    enable = true;
    fcitx5.waylandFrontend = true;
    # fcitx5.addons 无需手写 qingjian——模块内部已加（含 fcitx5-rime 等其他 addon 时照常并列）
  };
  environment.sessionVariables = {
    GTK_IM_MODULE = "fcitx";
    QT_IM_MODULE = "fcitx";
    XMODIFIERS = "@im=fcitx";
  };
}
```

启用后模块自动做两件事：把 `qingjian` 插件并进 `fcitx5.addons`；创建用户级
systemd 服务 `qingjian-server`（`graphical-session` 会话拉起，`QINGJIAN_RESOURCES`
注入数据根——官方 server 认的变量）。数据默认由 flake 内置（`qingjian-data`
解包为官方 data-v3 布局），可用 `services.qingjian.dataDir` 覆盖为自定义目录。

> 官方数据更新时 fork 的 `update-data` CI 会自动同步 URL 与 narHash 并推送，
> 本地只需 `nix flake lock --update-input qingjian-nixos` 拉回新 rev（不要用
> 全量 `nix flake update`，避免 nixpkgs 被动升级）。

### 作为 home-manager 模块引入（非 NixOS 发行版）

前提：发行版已装好 **Nix 包管理器** + **home-manager（standalone 模式）**，
以及 fcitx5 本体（`fcitx5`、`fcitx5-gtk`/`gtk4`、`fcitx5-qt`——用发行版自己的
包管理器装，home-manager 只管理青简自身）。

```nix
# ~/.config/home-manager/flake.nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    home-manager.url = "github:nix-community/home-manager";
    qingjian-nixos.url = "git+https://ghfast.top/https://github.com/Aozora-Wings/qingjian-nixos?ref=main";
  };
  outputs = { self, nixpkgs, home-manager, qingjian-nixos, ... }: {
    homeConfigurations.wt = home-manager.lib.homeManagerConfiguration {
      pkgs = import nixpkgs { system = "x86_64-linux"; };
      modules = [
        qingjian-nixos.homeManagerModules.default
        ({ ... }: { services.qingjian.enable = true; })
      ];
    };
  };
}
```

模块会：把插件部署到 `~/.local/share/fcitx5/addon/` + `~/.local/lib/fcitx5/`
（并设 `FCITX_LIBRARY_PATH` 让 fcitx5 找到库）；创建用户级 `qingjian-server`
服务；`QINGJIAN_RESOURCES` 指向内置数据根。

> 与 NixOS 的 `nixosModules.default` 二选一，不要同机同用户重复启用。

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
apps/linux/fcitx5      fcitx5 插件（C++，加载时连接 server）
modules/nixos.nix      NixOS 模块（fcitx5 addon + systemd user 服务）
modules/home.nix       home-manager 模块（standalone，非 NixOS 发行版）
packages/server.nix    server 打包（官方源码 + linux-workspace.lock）
packages/fcitx5.nix    插件打包
packages/linux-workspace.lock  完整 Cargo.lock（含 apps/linux/server 依赖）
```

（Rust server 本体来自官方 `inputs.qingjian` 源码，本仓库不再维护 `apps/linux/server` 副本。）

## License

GPL-3.0-or-later（跟随官方）。`apps/linux` 为对本仓库贡献的代码。
