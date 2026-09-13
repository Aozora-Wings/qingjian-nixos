{
  description = "青简输入法 Linux 版：Rust server + fcitx5 插件 + NixOS 模块";

  # 与 nixos-config 一致：nixpkgs 走南大镜像，避免 GitHub 直连不稳。
  inputs = {
    nixpkgs.url = "git+https://mirrors.nju.edu.cn/git/nixpkgs.git?ref=nixpkgs-unstable&shallow=1";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      qingjianServer = pkgs.callPackage ./packages/server.nix { };
      qingjianFcitx5 = pkgs.callPackage ./packages/fcitx5.nix { };
    in {
      packages.${system} = {
        inherit qingjianServer qingjianFcitx5;
        # 桌面整装：fcitx5 插件 + 服务模块。数据包（data/generated、data/model、assets）
        # 不进 git，由 nixos-config 以 flake=false 的二进制 input 提供，经
        # services.qingjian.dataDir 注入 QINGJIAN_DATA_DIR。
        default = qingjianFcitx5;
      };

      nixosModules.default = import ./modules/nixos.nix {
        inherit qingjianFcitx5 qingjianServer;
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
