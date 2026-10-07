# 青简 fcitx5 插件（官方源码 apps/linux/fcitx5）：InputMethodEngine 薄壳，
# 转发按键到 qingjian-server（Unix socket），DisplayOnlyCandidateList 展示候选。
# 源码随官方 main 演进（inputs.qingjian 锁定 rev），fork 不再维护本地副本；
# 官方更新时 check-upstream CI 自动同步。安装布局对齐 nixpkgs fcitx5 addon 规范：
#   $out/lib/fcitx5/qingjian.so
#   $out/share/fcitx5/addon/qingjian.conf
#   $out/share/fcitx5/inputmethod/qingjian.conf
{
  lib,
  stdenv,
  cmake,
  pkg-config,
  fcitx5,
  nlohmann_json,
  qingjianSrc,
}:
stdenv.mkDerivation {
  pname = "qingjian-fcitx5";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = "${qingjianSrc}/apps/linux/fcitx5";
    filter = path: type:
      !(type == "directory" && baseNameOf path == "build");
  };

  nativeBuildInputs = [ cmake pkg-config ];
  buildInputs = [ fcitx5 nlohmann_json ];

  # stdenv 检测到 cmake 后自动走 cmake configure/build/install 三阶段，
  # CMakeLists.txt 已声明 install 到 lib/fcitx5 + share/fcitx5/addon + share/fcitx5/inputmethod。
  # 官方 CMake 的 BUILD_TESTING 默认 ON，其中部分测试 target 需要已构建的 Rust
  # server 可执行文件——这里显式关掉测试，仅产插件本体（测试逻辑由官方 CI 覆盖）。
  cmakeFlags = [ "-DBUILD_TESTING=OFF" ];

  meta = {
    description = "青简输入法 fcitx5 插件（官方源码：显示候选、转发按键到 qingjian-server）";
    platforms = lib.platforms.linux;
  };
}
