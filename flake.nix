{
  description = "青简输入法 Linux 版：Rust server + fcitx5 插件 + NixOS 模块";

  # 与 nixos-config 一致：nixpkgs 走南大镜像，避免 GitHub 直连不稳。
  # 数据包（qingjian-data / qingjian-model）是上游 qingjian-team Release 资产，
  # 走 ghfast.top 加速镜像（境内可拉）；tag（当前 data-v1）由官方
  # tools/release/data.lock 控制，数据更新时由 CI（.github/workflows/update-data.yml）
  # 自动同步 URL 与 narHash。
  inputs = {
    nixpkgs.url = "git+https://mirrors.nju.edu.cn/git/nixpkgs.git?ref=nixpkgs-unstable&shallow=1";
    # 官方源码（crates 平台逻辑等；官方尚无 apps/linux，linux server/插件在本仓库维护）。
    # flake=false 拉 main tar（ghfast 镜像，境内可拉）；官方更新时
    # `nix flake lock --update-input qingjian` 即可跟进（无需 rebase 整树）。
    qingjian = {
      url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/archive/refs/heads/main.tar.gz";
      flake = false;
    };
    # 上游数据 tar.gz（扁平 dict/lm/glossary，重排由模块内 runCommand 完成）
    qingjian-data = {
      url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/releases/download/data-v1/qingjian-data.tar.gz";
      flake = false;
    };
    # 上游整句模型（单文件 model.qjm）
    qingjian-model = {
      url = "https://ghfast.top/https://github.com/qingjian-team/qingjian/releases/download/data-v1/model.qjm";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, ... }@inputs:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      qingjianServer = pkgs.callPackage ./packages/server.nix { qingjianSrc = inputs.qingjian; };
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

      # 生成完整 Cargo.lock（官方 lock 不含 apps/linux/server 依赖）。
      # 用 nixpkgs cargo（与 Nix 构建的 vendor 解析一致）；官方 main 或
      # nixpkgs 更新后需重新生成：`nix run .#gen-lock -- packages/linux-workspace.lock`
      apps.${system}.gen-lock = {
        type = "app";
        program = "${pkgs.writeShellScript "qingjian-gen-lock" ''
          set -euo pipefail
          OUT="${toString ./packages/linux-workspace.lock}"
          if [ $# -ge 1 ]; then OUT="$1"; fi
          # 解析为绝对路径（后面会 cd 进临时目录）
          OUT="$(realpath -m "$OUT")"
          export PATH=${pkgs.cargo}/bin:${pkgs.git}/bin:$PATH
          export CARGO_REGISTRIES_CRATES_IO_INDEX="sparse+https://rsproxy.cn/index/"
          TMP="$(mktemp -d)"
          cp -r ${inputs.qingjian}/. "$TMP"/
          chmod -R u+w "$TMP"
          cp -r ${./apps/linux} "$TMP/apps/linux/"
          chmod -R u+w "$TMP"
          rm -rf "$TMP/apps/linux/fcitx5/build" "$TMP/target" "$TMP/result"
          sed -i 's|"apps/windows/settings",|"apps/windows/settings", "apps/linux/server",|' "$TMP/Cargo.toml"
          sed -i '/^\[workspace.dependencies\]$/a libc = "0.2"' "$TMP/Cargo.toml"
          cd "$TMP"
          cargo generate-lockfile
          cp Cargo.lock "$OUT"
          rm -rf "$TMP"
          echo "lock 已生成：$OUT"
        ''}";
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
