{
  description = "A Rust dev shell using the latest stable toolchain";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    
    # Oxalica's rust-overlay tracks official upstream Rust channels
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, rust-overlay }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];

      forEachSupportedSystem = f: nixpkgs.lib.genAttrs supportedSystems (system: f {
        pkgs = import nixpkgs {
          inherit system;
          # Load the rust-overlay into pkgs
          overlays = [ rust-overlay.overlays.default ];
        };
      });
    in
    {
      devShells = forEachSupportedSystem ({ pkgs }:
        let
          # Select the latest stable channel and attach IDE tools
          rustToolchain = pkgs.rust-bin.stable.latest.default.override {
            extensions = [ "rust-src" "rust-analyzer" ];
          };
        in
        {
          default = pkgs.mkShell {
            # Darwin + strictDeps hides buildInputs from pkg-config, so
            # audiopus_sys misses libopus and compiles a vendored copy with CMake.
            strictDeps = false;

            nativeBuildInputs = [
              rustToolchain # Includes rustc, cargo, rustfmt, clippy, rust-analyzer
              pkgs.pkg-config
              pkgs.cmake
              pkgs.libopus
            ];

            buildInputs = [
              pkgs.libopus
            ];

            # Set path so editor LSP integrations can read standard library source code
            RUST_SRC_PATH = "${rustToolchain}/lib/rustlib/src/rust/library";
            # audiopus_sys uses these as the *prefix* (it appends /lib). If they
            # are unset and pkg-config fails, it builds Opus with CMake.
            LIBOPUS_LIB_DIR = "${pkgs.libopus}";
            OPUS_LIB_DIR = "${pkgs.libopus}";
          };
        });
    };
}