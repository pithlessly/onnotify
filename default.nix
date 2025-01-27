{ pkgs ? import <nixpkgs> {} }:
pkgs.stdenv.mkDerivation {
  name = "onnotify";
  src = ./.;
  buildPhase = ''
    $CC notifybycwd.c -o notifybycwd
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp notifybycwd $out/bin
    cp onnotify.py $out/bin/onnotify
    chmod +x $out/bin/onnotify
  '';
}
