# SPDX-License-Identifier: LGPL-3.0-only
{ lib
, stdenv
, cmake
, ninja
, pkg-config
, qt6
, kdePackages
, vips
, libwebp
, makeWrapper
, runCommand
, mpv-unwrapped
, withVideo ? false
}:
let
  mpvHeaders = runCommand "imvqml-mpv-headers-${mpv-unwrapped.version}" { } ''
    mkdir -p "$out/include"
    cp -r ${lib.getDev mpv-unwrapped}/include/mpv "$out/include/"
  '';
in
stdenv.mkDerivation {
  pname = "imvqml";
  version = "0.1.2";
  src = lib.cleanSource ./.;
  nativeBuildInputs = [ cmake ninja pkg-config makeWrapper ];
  buildInputs = [ qt6.qtbase qt6.qtdeclarative qt6.qtimageformats kdePackages.kimageformats vips libwebp ];
  cmakeFlags = [
    "-DBUILD_TESTING=OFF"
    "-DIMV_MPV_HEADERS=${mpvHeaders}/include"
  ] ++ lib.optionals withVideo [
      "-DIMV_MPV_RUNTIME_HINT=${lib.getLib mpv-unwrapped}/lib/libmpv.so.2"
  ];
  dontWrapQtApps = true;
  postFixup = ''
    wrapProgram "$out/libexec/imvqml-decoder" \
      --prefix QT_PLUGIN_PATH : '${lib.makeSearchPath qt6.qtbase.qtPluginPrefix [ qt6.qtimageformats kdePackages.kimageformats ]}'
  '';
  meta = {
    description = "Qt Quick image pipeline with an isolated decoder";
    license = lib.licenses.lgpl3Only;
    platforms = lib.platforms.linux;
  };
}
