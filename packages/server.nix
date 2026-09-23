# 青简 Linux Rust server：独立进程跑 Engine，在 Unix domain socket 上服务 fcitx5 插件。
# 数据（data/generated、data/model、assets）不进 git、不随本包，由 NixOS 侧以
# QINGJIAN_DATA_DIR 环境变量注入（见 modules/nixos.nix 的 services.qingjian.dataDir）。
#
# 源码策略（方案 A：server 由官方提供）：官方 apps/linux/server 已在官方 workspace
# （官方 Cargo.toml members 含 apps/linux/server，Cargo.lock 含其依赖），因此本包
# 直接用 flake input `qingjian`（flake=false）的官方源码构建官方 server，本地不再
# 维护/覆盖 apps/linux/server。本地仓库只维护 apps/linux/fcitx5（fcitx5 引导插件，
# 见 packages/fcitx5.nix）。官方更新 → `nix flake lock --update-input qingjian`
# 跟进；cargo 依赖变化时按构建报错更新 cargoHash（CI 已自动处理）。
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

  # 官方源码 store 只读；cargo 构建只需读取，这里仅放开写权限以防官方输入在
  # 构建期需要写（如生成文件），与旧合并逻辑行为保持一致。
  src = pkgs.runCommand "qingjian-linux-src" { } ''
    cp -r ${qingjianSrc}/. $out/
    chmod -R u+w $out
    rm -rf $out/apps/linux/fcitx5/build $out/target $out/result
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

  cargoHash = "sha256-f4qC8SHYaoh3ji8eRp/W5ik601VWoSni+MCnOFfqSMs=";

  # 依赖下载已在 fetchCargoVendor（镜像版）阶段完成，构建期 cargo 由
  # cargoSetupHook 自动配置使用 vendored 依赖，无需再写 registry 配置。

  # buildRustPackage 默认装 workspace 全部 bin；这里只装 server。
  # 注意 cargo 带 --target 构建，产物在 target/<triple>/release/ 下。
  # 官方二进制名为 qingjian-linux-server；安装时保持 qingjian-server 名，
  # 与 modules/nixos.nix 的 ExecStart 引用（${qingjianServer}/bin/qingjian-server）一致。
  installPhase = ''
    mkdir -p "$out/bin"
    cp target/x86_64-unknown-linux-gnu/release/qingjian-linux-server "$out/bin/qingjian-server"
  '';

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ openssl ];

  meta = {
    description = "青简输入法 Linux server（Rust）：Unix socket IPC + 本地组句/选词/整句模型";
    mainProgram = "qingjian-server";
  };
}
