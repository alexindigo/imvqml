# SPDX-License-Identifier: LGPL-3.0-only
{ lib
, stdenv
, cmake
, ninja
, pkg-config
, qt6
, vips
, libwebp
, mpv-unwrapped ? null
, withVideo ? false
}:
assert withVideo -> mpv-unwrapped != null;
stdenv.mkDerivation {
  pname = "imvqml";
  version = "0.1.0";
  src = lib.cleanSource ./.;
  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [ qt6.qtbase qt6.qtdeclarative vips libwebp ]
    ++ lib.optionals withVideo [ mpv-unwrapped ];
  cmakeFlags = [ "-DBUILD_TESTING=OFF" ]
    ++ (if withVideo then [
      "-DIMV_ENABLE_VIDEO=ON"
      "-DIMV_MPV_HEADERS=${lib.getDev mpv-unwrapped}/include"
      "-DIMV_MPV_RUNTIME_HINT=${lib.getLib mpv-unwrapped}/lib/libmpv.so.2"
    ] else [ "-DIMV_ENABLE_VIDEO=OFF" ]);
  dontWrapQtApps = true;
  meta = {
    description = "Qt Quick image pipeline with an isolated decoder";
    license = lib.licenses.lgpl3Only;
    platforms = lib.platforms.linux;
  };
}
