{
  description = "青简输入法 Linux 版：Rust server + fcitx5 插件 + NixOS 模块";

  # 与 nixos-config 一致：nixpkgs 走南大镜像，避免 GitHub 直连不稳。
  # 数据包（qingjian-data / qingjian-model）是上游 qingjian-team Release 资产，
  # 走 ghfast.top 加速镜像（境内可拉）；flake=false 锁定 URL，更新时
  # `nix flake lock --update-input qingjian-data` 即可。
  inputs = {
    nixpkgs.url = "git+https://mirrors.nju.edu.cn/git/nixpkgs.git?ref=nixpkgs-unstable&shallow=1";
    # 上游数据 tar.gz（扁平 dict/lm/glossary，重排由模块内 runCommand 完成）
    qingjian-data = {
      url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/releases/download/data/qingjian-data.tar.gz";
      flake = false;
    };
    # 上游整句模型（单文件 model.qjm）
    qingjian-model = {
      url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/releases/download/data/model.qjm";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, ... }@inputs:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      qingjianServer = pkgs.callPackage ./packages/server.nix { };
      qingjianFcitx5 = pkgs.callPackage ./packages/fcitx5.nix { };
    in {
      packages.${system} = {
        inherit qingjianServer qingjianFcitx5;
        # 桌面整装：fcitx5 插件 + 服务模块。
        default = qingjianFcitx5;
      };

      nixosModules.default = import ./modules/nixos.nix {
        inherit qingjianFcitx5 qingjianServer;
        # 数据包由本 flake 自带：模块内默认组装 data/generated + data/model，
        # 启用 services.qingjian.enable 即默认载入，无需 nixos-config 侧提供。
        dataPackage = inputs."qingjian-data";
        modelPackage = inputs."qingjian-model";
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
