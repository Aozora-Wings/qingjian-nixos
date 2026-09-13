# 青简 Linux Rust server：独立进程跑 Engine，在 Unix domain socket 上服务 fcitx5 插件。
# 数据（data/generated、data/model、assets）不进 git、不随本包，由 NixOS 侧以
# QINGJIAN_DATA_DIR 环境变量注入（见 modules/nixos.nix 的 services.qingjian.dataDir）。
{
  lib,
  rustPlatform,
  openssl,
  pkg-config,
  pkgs,
}:
let
  # 境内网络：nix 的 fetch-cargo-vendor 默认从 static.crates.io 官方 CDN 下载 crate
  # （直连不稳会挂起），这里换成 rsproxy 镜像版——packages/fetch-cargo-vendor* 是
  # nixpkgs 原样拷贝，仅把下载 URL 换成 rsproxy.cn（checksum 不变，内容一致）。
  fetchCargoVendor = pkgs.callPackage ./fetch-cargo-vendor.nix { };
  rustPlatform' = rustPlatform.buildRustPackage.override { inherit fetchCargoVendor; };
in
rustPlatform' {
  pname = "qingjian-server";
  version = "0.1.0-alpha.1-dev";

  # 整个仓库（workspace 根）为 src：server 通过 path 依赖 crates/*。
  # 排除数据、构建产物、插件源码与 .githooks 之外的杂项。
  src = lib.cleanSourceWith {
    src = ../.;
    filter = path: type:
      let
        base = baseNameOf path;
        rel = lib.removePrefix (toString ../. + "/") (toString path);
      in
      base != "data"
      && base != "target"
      && base != "result"
      && base != "downloads"
      && base != ".githooks"
      # apps/linux/fcitx5 由 fcitx5 包构建，这里不重复带。
      && !(lib.hasPrefix "apps/linux/fcitx5" rel);
  };

  # workspace 里 macOS 专用 crate（qingjian-macos 依赖 objc2）在 Linux 上无法编译，
  # 只构建/测试 server 包本身。注意：新版 buildRustPackage 的 cargo 钩子从**环境变量**
  # 读这些 flags（不自动传递同名参数）。
  env = {
    cargoBuildFlags = "-p qingjian-linux-server";
    # 注意：check hook 读的是 cargoTestFlags（不是 cargoCheckFlags）。
    cargoTestFlags = "-p qingjian-linux-server";
  };

  cargoHash = "sha256-KwRui6mXABgwpcxTP3Afrn5m6gs3IQwRG7adiiEmzvk=";

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
