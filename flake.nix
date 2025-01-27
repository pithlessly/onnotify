{
  description = "Program to run a shell command on any notification";

  inputs.flake-utils.url = "github:numtide/flake-utils";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, flake-utils, nixpkgs }:
  flake-utils.lib.eachDefaultSystem (system:
  let
    pkgs = nixpkgs.legacyPackages.${system};
    onnotify = import ./default.nix { inherit pkgs; };
  in {
    packages.onnotify = onnotify;
    defaultPackage = onnotify;
  });
}
