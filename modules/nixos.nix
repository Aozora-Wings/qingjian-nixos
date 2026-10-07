# 青简 NixOS 模块：
#   1) 把 fcitx5 插件并进 `i18n.inputMethod.fcitx5.addons`（桌面机共用）；
#   2) 以用户级 systemd 服务拉起 qingjian-server，并把数据根以
#      QINGJIAN_RESOURCES 注入（官方 server 认的环境变量；数据默认由本 flake 的
#      qingjian-data input 解包出官方 data-v3 布局：data/generated + data/models/
#      hanzhang-* + assets，启用即默认载入；需要自定义时用 services.qingjian.dataDir 覆盖）。
# 用法（nixos-config）：
#   qingjian.url = "github:Aozora-Wings/qingjian-nixos";
#   （启用处）imports = [ inputs.qingjian.nixosModules.default ];
#   services.qingjian.enable = true;
{ qingjianFcitx5, qingjianServer, dataPackage }:
{ config, lib, pkgs, ... }:
let
  cfg = config.services.qingjian;

  # 默认数据根：上游 release 数据包（官方 data-v3 布局）原样解包即资源根——
  # data/generated（dict/lm/glossary）+ data/models/hanzhang-*（整句模型）+
  # assets/（可选，缺失自动降级）。store 输入只读，复制后放开写权限再清理
  # macOS AppleDouble 冗余（._ 前缀）。
  assembledData = pkgs.runCommand "qingjian-data" { } ''
    mkdir -p $out
    cp -r ${dataPackage}/. $out/
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
        Environment = "QINGJIAN_RESOURCES=${cfg.dataDir}";
      };
    };
  };
}
