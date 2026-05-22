{
  description = "Program to run a shell command on any notification";

  inputs.flake-utils.url = "github:numtide/flake-utils";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, flake-utils, nixpkgs }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        drv = pkgs.callPackage ./default.nix {};
      in {
        packages.onnotify = drv;
        defaultPackage = drv;
      }
    ) // {
      overlays.default = final: prev: {
        onnotify = final.callPackage ./default.nix {};
      };
    };
}
