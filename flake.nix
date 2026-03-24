{
  description = "IncludeOS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/25.05";
  };

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";

      mkIncludeos = { withCcache ? false, smp ? false }:
        let
          overlays = [
            (import ./overlay.nix {
              inherit withCcache smp;
              disableTargetWarning = true;
            })
          ];
          pkgs            = import nixpkgs { config = {}; inherit overlays; };
          pkgsIncludeOS   = pkgs.pkgsIncludeOS;
          stdenvIncludeOS = pkgs.stdenvIncludeOS;
          includeos       = pkgsIncludeOS.includeos;
        in {
          inherit stdenvIncludeOS;  # the custom stdenv scope used by includeos (llvm, musl variants, libcxx)
          inherit pkgsIncludeOS;    # the musl/clang package scope used by IncludeOS
          inherit pkgs;             # nixpkgs with the IncludeOS overlay applied
          inherit includeos;        # the IncludeOS derivation to add to buildInputs
        };

      default = mkIncludeos {};
    in {
      packages.${system} = {
        default     = default.includeos;           # the default IncludeOS (no SMP, no ccache)
        chainloader = import ./chainloader.nix {}; # the chainloader "unikernel" stub that hotswaps from 32-bit to 64-bit unikernel
      };

      devShells.${system}.default = import ./develop.nix { includeos = default.includeos; };

      lib.${system} = {
        inherit mkIncludeos;

        # build a unikernel against IncludeOS
        mkUnikernel = args:
          import ./unikernel.nix ({ includeos = default.includeos; } // args);
      };
    };
}
