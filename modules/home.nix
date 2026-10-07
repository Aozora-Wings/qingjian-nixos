# 青简 home-manager 模块（standalone，非 NixOS 发行版用）：
#   1) 把 fcitx5 插件部署到用户目录（addon 配置 → ~/.local/share/fcitx5/addon，
#      共享库 → ~/.local/lib/fcitx5，并用 FCITX_LIBRARY_PATH 让 fcitx5 找到库）；
#   2) 以用户级 systemd 服务拉起 qingjian-server，QINGJIAN_RESOURCES 指向数据根
#      （官方 server 认的环境变量）。
#
# 配置哲学（混合式）：基础设施（插件/服务/数据源）→ 本模块声明式 options；
# 个人偏好（词库开关、按键、候选样式等）→ 官方运行时 ~/.config/qingjian/config.toml
# （改完重启 qingjian-server 生效，官方 Linux 无热加载）。想声明式管默认值的用户
# 可设 initialConfigFile：首次部署写入一份真实文件，之后仍可运行时修改，不覆盖。
#
# 前提：发行版已装好 fcitx5 本体 + 各框架桥接包（fcitx5、fcitx5-gtk/gtk4、fcitx5-qt）。
# 用法（其他发行版：Arch/Ubuntu/Fedora… 装了 Nix + home-manager standalone）：
#   { pkgs, inputs, ... }:
#   {
#     imports = [ inputs.qingjian-nixos.homeManagerModules.default ];
#     services.qingjian.enable = true;
#   }
# 注意：与 NixOS 的 nixosModules.default 二选一，不要同机同用户重复启用。
{ qingjianFcitx5, qingjianServer, dataPackage }:
{ config, lib, pkgs, ... }:
let
  cfg = config.services.qingjian;

  # 默认数据根：上游 release 数据包（官方 data-v3 布局）原样解包即资源根——
  # data/generated（dict/lm/glossary）+ data/models/hanzhang-*（整句模型）+
  # assets/（可选，缺失自动降级）。store 输入只读，复制后放开写权限再清理
  # macOS AppleDouble 冗余（._ 前缀）。
  assembledData = pkgs.runCommand "qingjian-data" { } ''
    # 同上：nix flake=false input 剥离 tar 单层顶层目录，补回 data/ 层作为资源根。
    mkdir -p $out/data
    cp -r ${dataPackage}/. $out/data/
    chmod -R u+w $out
    find $out -name '._*' -delete
  '';
in
{
  options.services.qingjian = {
    enable = lib.mkEnableOption "青简输入法（fcitx5 插件 + 本地 Rust server）";

    dataDir = lib.mkOption {
      type = lib.types.path;
      default = assembledData;
      defaultText = lib.literalExpression "fork 内置数据包组装（qingjian-data，官方 data-v3 布局）";
      description = ''
        青简资源根目录（含 data/generated、data/models/hanzhang-*、assets 的子目录，
        即官方 server 的 QINGJIAN_RESOURCES 所指）。默认由本 flake 的 qingjian-data
        input 解包组装；需要换用其他数据源时覆盖为自定义目录。
      '';
    };

    package = lib.mkOption {
      type = lib.types.package;
      default = qingjianFcitx5;
      defaultText = lib.literalExpression "qingjian-fcitx5";
      description = "fcitx5 addon 包（lib/fcitx5 + share/fcitx5/addon 布局）。";
    };

    serverPackage = lib.mkOption {
      type = lib.types.package;
      default = qingjianServer;
      defaultText = lib.literalExpression "qingjian-server";
      description = "qingjian-server 包（bin/qingjian-server）。";
    };

    serverArgs = lib.mkOption {
      type = lib.types.listOf lib.types.str;
      default = [ ];
      example = [ "--log-level" "debug" ];
      description = "追加传给 qingjian-server 的命令行参数。";
    };

    extraEnvironment = lib.mkOption {
      type = lib.types.attrsOf lib.types.str;
      default = { };
      example = { RUST_LOG = "debug"; };
      description = "追加注入服务进程的环境变量（QINGJIAN_RESOURCES 已默认设置）。";
    };

    initialConfigFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      description = ''
        可选的初始配置（官方 config.toml）。设置后首次部署写入 ~/.config/qingjian/config.toml；
        文件已存在时**不会覆盖**（保留运行时修改）。不设置则完全走官方运行时默认。
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    # 1) fcitx5 插件：addon 配置走 XDG_DATA_HOME（fcitx5 自动扫描
    #    ~/.local/share/fcitx5/addon/*.conf）
    xdg.dataFile."fcitx5/addon/qingjian.conf".source =
      "${cfg.package}/share/fcitx5/addon/qingjian.conf";

    # 2) 插件共享库：fcitx5 不扫 ~/.local/lib/fcitx5，需显式用 FCITX_LIBRARY_PATH 指路
    home.file.".local/lib/fcitx5/libqingjian.so".source =
      "${cfg.package}/lib/fcitx5/libqingjian.so";
    home.sessionVariables.FCITX_LIBRARY_PATH = "$HOME/.local/lib/fcitx5";

    # 3) 首次部署注入初始 config.toml（home.activation：真实文件、已存在不覆盖，
    #    保留运行时修改——声明式默认 + 运行时自由）
    home.activation.qingjianInitConfig = lib.mkIf (cfg.initialConfigFile != null)
      (lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        if [ ! -e "$HOME/.config/qingjian/config.toml" ]; then
          mkdir -p "$HOME/.config/qingjian"
          install -m 600 ${cfg.initialConfigFile} "$HOME/.config/qingjian/config.toml"
        fi
      '');

    # 4) 用户级服务：与 fcitx5 同 graphical-session（home-manager 的 systemd 用标准
    #    Unit/Service/Install 三段式，与 NixOS 模块的 serviceConfig 写法不同）
    systemd.user.services.qingjian-server = {
      Unit = {
        Description = "qingjian 输入法 Rust server";
        After = [ "graphical-session.target" ];
        PartOf = [ "graphical-session.target" ];
        StartLimitIntervalSec = 10;
        StartLimitBurst = 5;
      };
      Service = {
        Type = "simple";
        ExecStart = "${cfg.serverPackage}/bin/qingjian-server"
          + lib.optionalString (cfg.serverArgs != [ ])
            (" " + lib.concatStringsSep " " cfg.serverArgs);
        Restart = "on-failure";
        RestartSec = "2";
        Environment =
          [ "QINGJIAN_RESOURCES=${cfg.dataDir}" ]
          ++ lib.mapAttrsToList (name: value: "${name}=${value}") cfg.extraEnvironment;
      };
      Install = {
        WantedBy = [ "graphical-session.target" ];
      };
    };
  };
}
