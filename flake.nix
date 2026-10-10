{
  description = "青简输入法 Linux 版：Rust server + fcitx5 插件 + NixOS 模块";

  # 与 nixos-config 一致：nixpkgs 走南大镜像，避免 GitHub 直连不稳。
  # 数据包（qingjian-data）是上游 qingjian-team Release 资产（data-v3 起模型并入
  # tar 的 data/models/hanzhang-*/，不再单独发布 model.qjm）；走 ghfast.top 加速
  # 镜像（境内可拉）；tag 由官方 tools/release/data.lock 控制，数据更新时由 CI
  # （.github/workflows/update-data.yml）自动同步 URL 与 narHash。
  inputs = {
    nixpkgs.url = "git+https://mirrors.nju.edu.cn/git/nixpkgs.git?ref=nixpkgs-unstable&shallow=1";
    # 官方源码（crates 平台逻辑 + apps/linux server/插件；fork 只做 Nix 分发层）。
    # git 方式锁定 rev（官方 main 更新后旧 rev 稳定，不会像 tarball 那样内容变了报 narHash mismatch）；
    # 官方更新时 `nix flake lock --update-input qingjian` 跟进。
    qingjian = {
      url = "git+https://ghfast.top/https://github.com/qingjian-team/qingjian?ref=main&shallow=1";
      flake = false;
    };
    # 上游数据 tar.gz（data/generated 词库 + data/models 整句模型 + assets，官方约定布局）
    qingjian-data = {
      url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/releases/download/data-v4/qingjian-data.tar.gz";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, ... }@inputs:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      qingjianServer = pkgs.callPackage ./packages/server.nix { qingjianSrc = inputs.qingjian; };
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

        # Rust server：rustup 管 toolchain（rust-toolchain.toml 指定 1.96.0）。
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
