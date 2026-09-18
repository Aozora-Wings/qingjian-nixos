# 青简 Linux Rust server：独立进程跑 Engine，在 Unix domain socket 上服务 fcitx5 插件。
# 数据（data/generated、data/model、assets）不进 git、不随本包，由 NixOS 侧以
# QINGJIAN_DATA_DIR 环境变量注入（见 modules/nixos.nix 的 services.qingjian.dataDir）。
#
# 源码策略（瘦身后的集成仓库）：本仓库不再 fork 官方整树——
#   官方源码由 flake input `qingjian`（flake=false）提供；
#   本地仓库只维护 apps/linux（linux server + fcitx5 插件源码）；
#   构建时把两者合并：官方 crates 提供平台逻辑，apps/linux/server 挂进官方
#   workspace（patch members + 补 libc 依赖），Cargo.lock 用预生成的
#   packages/linux-workspace.lock（含 apps/linux/server 依赖，官方 lock 没有）。
#   官方更新 → `nix flake lock --update-input qingjian` 跟进，无需 rebase。
{
  lib,
  rustPlatform,
  openssl,
  pkg-config,
  pkgs,
  qingjianSrc,
}:
let
  # 境内网络：nix 的 fetch-cargo-vendor 默认从 static.crates.io 官方 CDN 下载 crate
  # （直连不稳会挂起），这里换成 rsproxy 镜像版——packages/fetch-cargo-vendor* 是
  # nixpkgs 原样拷贝，仅把下载 URL 换成 rsproxy.cn（checksum 不变，内容一致）。
  fetchCargoVendor = pkgs.callPackage ./fetch-cargo-vendor.nix { };
  rustPlatform' = rustPlatform.buildRustPackage.override { inherit fetchCargoVendor; };

  # 合并源码：官方仓库（crates/apps/tools）+ 本地 apps/linux。
  src = pkgs.runCommand "qingjian-linux-src" { } ''
    cp -r ${qingjianSrc}/. $out/
    chmod -R u+w $out
    cp -r ${../apps/linux} $out/apps/linux/
    # 本地 path 是 store 只读，cp 后需再放开写权限
    chmod -R u+w $out
    # 本地 path 引用可能带入的构建产物，清掉
    rm -rf $out/apps/linux/fcitx5/build $out/target $out/result
    # 把 linux server 注册进官方 workspace（members + 官方已移除的 libc）
    sed -i 's|"apps/windows/settings",|"apps/windows/settings", "apps/linux/server",|' $out/Cargo.toml
    sed -i '/^\[workspace.dependencies\]$/a libc = "0.2"' $out/Cargo.toml
    # 官方 lock 不含 apps/linux/server 依赖：用预生成的完整 lock 覆盖
    # （packages/linux-workspace.lock，由与 nixpkgs 构建相同的 cargo 版本生成；
    #  官方/nixpkgs 更新后需重新生成，见仓库 README/CI）。
    cp ${./linux-workspace.lock} $out/Cargo.lock
    # 校验覆盖生效（防止 flake path 快照旧导致构建用官方 lock）
    grep -q 'qingjian-linux-server' $out/Cargo.lock || { echo "FATAL: linux-workspace.lock 覆盖未生效"; exit 1; }
  '';
in
rustPlatform' {
  pname = "qingjian-server";
  version = "0.1.0-alpha.1-dev";
  inherit src;

  # workspace 里 macOS 专用 crate（qingjian-macos 依赖 objc2）在 Linux 上无法编译，
  # 只构建/测试 server 包本身。注意：新版 buildRustPackage 的 cargo 钩子从**环境变量**
  # 读这些 flags（不自动传递同名参数）。
  env = {
    cargoBuildFlags = "-p qingjian-linux-server";
    # 注意：check hook 读的是 cargoTestFlags（不是 cargoCheckFlags）。
    cargoTestFlags = "-p qingjian-linux-server";
  };

  cargoHash = "sha256-KKPY96BwNQ7alIww/LamMu1wDd3w13YOTvgmS3jzegQ=";

  # 依赖下载已在 fetchCargoVendor（镜像版）阶段完成，构建期 cargo 由
  # cargoSetupHook 自动配置使用 vendored 依赖，无需再写 registry 配置。

  # buildRustPackage 默认装 workspace 全部 bin；这里只装 server。
  # 注意 cargo 带 --target 构建，产物在 target/<triple>/release/ 下。
  installPhase = ''
    mkdir -p "$out/bin"
    cp target/x86_64-unknown-linux-gnu/release/qingjian-server "$out/bin/"
  '';

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ openssl ];

  meta = {
    description = "青简输入法 Linux server（Rust）：Unix socket IPC + 本地组句/选词/整句模型";
    mainProgram = "qingjian-server";
  };
}
