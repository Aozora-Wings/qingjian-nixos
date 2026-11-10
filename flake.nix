{
  description = "青简输入法 Linux 版：Rust server + fcitx5 插件 + NixOS 模块";

  # 与 nixos-config 一致：nixpkgs 走南大镜像，避免 GitHub 直连不稳。
  # 数据包（qingjian-data）是上游 qingjian-team Release 资产（data-v3 起模型并入
  # tar 的 data/models/hanzhang-*/，不再单独发布 model.qjm）；走 ghfast.top 加速
  # 镜像（境内可拉）；tag 由官方 tools/release/data.lock 控制，数据更新时由 CI
  # （.github/workflows/update-data.yml）自动同步 URL 与 narHash。
  inputs = {
    nixpkgs.url = "git+https://mirrors.nju.edu.cn/git/nixpkgs.git?ref=nixpkgs-unstable&shallow=1";

    # Rust 工具链来源：上游 rust-toolchain.toml 固定 channel = "1.96.0"，构建必须
    # 与之对齐。nixpkgs 自带的 rustc 1.97.x 在 core_arch 里把
    # llvm.x86.avx512.vpdpbusd.256 声明成 (v8i32, v32i8, v32i8)，与 LLVM 21 期望的
    # (v8i32, v8i32, v8i32) 不符，candle-core 0.11.1 的 quantized/repack_x86.rs
    # （AVX512-VNNI 内核，x86_64 下无条件编译）会直接报
    # "intrinsic signature mismatch"，整个 server 构建失败。
    # 官方更新 rust-toolchain.toml 时，这里的版本要同步改。
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    # 官方源码（crates 平台逻辑 + apps/linux server/插件；fork 只做 Nix 分发层）。
    # git 方式锁定 rev（官方 main 更新后旧 rev 稳定，不会像 tarball 那样内容变了报 narHash mismatch）；
    # 官方更新时 `nix flake lock --update-input qingjian` 跟进。
    qingjian = {
      url = "git+https://ghfast.top/https://github.com/qingjian-team/qingjian?ref=main&shallow=1";
      flake = false;
    };
    # 上游数据 tar.gz（data/generated 词库 + data/models 整句模型 + assets，官方约定布局）
    qingjian-data = {
      url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/releases/download/data-v10/qingjian-data.tar.gz";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, rust-overlay, ... }@inputs:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ (import rust-overlay) ];
      };

      # 与上游 rust-toolchain.toml 的 channel = "1.96.0" 一致；改上游版本时改这里。
      rustToolchain = pkgs.rust-bin.stable."1.96.0".default;
      rustPlatform = pkgs.makeRustPlatform {
        cargo = rustToolchain;
        rustc = rustToolchain;
      };

      qingjianServer = pkgs.callPackage ./packages/server.nix {
        inherit rustPlatform;
        qingjianSrc = inputs.qingjian;
      };
      qingjianFcitx5 = pkgs.callPackage ./packages/fcitx5.nix { qingjianSrc = inputs.qingjian; };
    in {
      packages.${system} = {
        inherit qingjianServer qingjianFcitx5;
        # 桌面整装：fcitx5 插件 + 服务模块。
        default = qingjianFcitx5;
      };

      nixosModules.default = import ./modules/nixos.nix {
        inherit qingjianFcitx5 qingjianServer;
        # 数据包由本 flake 自带：模块内默认解包 data-v3 官方布局（data/generated +
        # data/models/hanzhang-* + assets）为资源根，启用 services.qingjian.enable
        # 即默认载入，无需 nixos-config 侧提供。
        dataPackage = inputs."qingjian-data";
      };

      # 非 NixOS 发行版（Arch/Ubuntu/Fedora…）：装了 Nix + home-manager standalone
      # 即可用，部署插件到用户 fcitx5 目录并拉起用户级 server 服务。
      homeManagerModules.default = import ./modules/home.nix {
        inherit qingjianFcitx5 qingjianServer;
        dataPackage = inputs."qingjian-data";
      };

      devShells.${system}.default = pkgs.mkShell {
        name = "qingjian-linux-dev";

        # Rust server：rustup 管 toolchain（rust-toolchain.toml 指定 1.96.0），
        # 版本要与上面的 rustToolchain 保持一致（构建用的是固定 1.96.0）。
        # fcitx5 插件：gcc/cmake/pkg-config + fcitx5 开发头与 CMake 模块。
        packages = with pkgs; [
          rustup
          gcc
          cmake
          pkg-config
        ];
        buildInputs = with pkgs; [
          # fcitx5 包内含 fcitx-utils 头与 Fcitx5Core/Fcitx5Utils CMake 模块。
          fcitx5
          # Rust 依赖链：openssl-sys 等需要系统库头。
          openssl
        ];

        shellHook = ''
          # 首次进入时按仓库 rust-toolchain.toml 安装工具链（装过则直接复用）。
          rustup toolchain install 1.96.0 --profile minimal 2>/dev/null || true
          export PATH="$HOME/.cargo/bin:$PATH"
        '';
      };
    };
}
