# 青简 fcitx5 插件：InputMethodEngine 薄壳，转发按键到 qingjian-server（Unix socket），
# 用 DisplayOnlyCandidateList 展示候选。安装布局对齐 nixpkgs fcitx5 addon 规范：
#   $out/lib/fcitx5/qingjian.so
#   $out/share/fcitx5/addon/qingjian.conf
# 这样 `i18n.inputMethod.fcitx5.addons` 直接收录即生效。
{
  lib,
  stdenv,
  cmake,
  pkg-config,
  fcitx5,
}:
stdenv.mkDerivation {
  pname = "qingjian-fcitx5";
  version = "0.1.0-alpha.1-dev";

  src = lib.cleanSourceWith {
    src = ../apps/linux/fcitx5;
    filter = path: type:
      !(type == "directory" && baseNameOf path == "build");
  };

  nativeBuildInputs = [ cmake pkg-config ];
  buildInputs = [ fcitx5 ];

  # stdenv 检测到 cmake 后自动走 cmake configure/build/install 三阶段，
  # CMakeLists.txt 已声明 install 到 lib/fcitx5 + share/fcitx5/addon。

  meta = {
    description = "青简输入法 fcitx5 插件（显示候选、转发按键到 qingjian-server）";
    platforms = lib.platforms.linux;
  };
}
