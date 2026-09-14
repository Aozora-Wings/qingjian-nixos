# 青简 NixOS 模块：
#   1) 把 fcitx5 插件并进 `i18n.inputMethod.fcitx5.addons`（桌面机共用）；
#   2) 以用户级 systemd 服务拉起 qingjian-server，并把数据目录以
#      QINGJIAN_DATA_DIR 注入（数据由 nixos-config 的 flake=false 二进制 input 提供）。
# 用法（nixos-config）：
#   qingjian.url = "github:Aozora-Wings/qingjian-nixos";
#   （启用处）imports = [ inputs.qingjian.nixosModules.default ];
#   services.qingjian = {
#     enable = true;
#     dataDir = inputs.qingjian-data;   # flake=false 的 release 数据包
#   };
{ qingjianFcitx5, qingjianServer }:
{ config, lib, ... }:
let
  cfg = config.services.qingjian;
in
{
  options.services.qingjian = {
    enable = lib.mkEnableOption "青简输入法（fcitx5 插件 + 本地 Rust server）";

    dataDir = lib.mkOption {
      type = lib.types.path;
      description = ''
        青简产品数据根目录（含 data/generated、data/model、assets 的子目录）。
        由 nixos-config 以 flake=false 的二进制 input 提供，如
        `qingjian-data.url = "https://<镜像>/.../qingjian-data.tar.gz"; flake = false;`
      '';
    };

    package = lib.mkOption {
      type = lib.types.package;
      default = qingjianFcitx5;
      defaultText = lib.literalExpression "qingjian-fcitx5";
      description = "fcitx5 addon 包（lib/fcitx5 + share/fcitx5/addon 布局）。";
    };
  };

  config = lib.mkIf cfg.enable {
    i18n.inputMethod.fcitx5.addons = [ cfg.package ];

    # 用户级服务：与 fcitx5 同会话（graphical-session），重启即拉起。
    # 注意：NixOS systemd 服务的 option 是顶层小写属性 + serviceConfig/unitConfig，
    # 没有 Unit/Service/Install 子段（那会报 option 不存在）。
    systemd.user.services.qingjian-server = {
      description = "qingjian 输入法 Rust server";
      wantedBy = [ "graphical-session.target" ];
      after = [ "graphical-session.target" ];
      partOf = [ "graphical-session.target" ];
      # nixpkgs 26.x 这两个 option 无默认值但 unit 生成时会读取，需显式赋值（对齐 systemd 原生默认）
      startLimitIntervalSec = 10;
      startLimitBurst = 5;
      serviceConfig = {
        Type = "simple";
        ExecStart = "${qingjianServer}/bin/qingjian-server";
        Restart = "on-failure";
        RestartSec = "2";
        Environment = "QINGJIAN_DATA_DIR=${cfg.dataDir}";
      };
    };
  };
}
