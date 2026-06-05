{
  description = "shimmy — single-host, shared-memory, SPMC messaging library (C++20)";

  # Pinned for reproducible benchmark numbers. "Data doesn't lie" requires a
  # fixed toolchain — see DESIGN.md §7/§8. The flake.lock pins the exact
  # nixpkgs revision; the input ref below is the human-readable channel it
  # tracks. Do not bump casually: a toolchain change can move the numbers.
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # HdrHistogram_c is NOT packaged in nixpkgs. We pin a specific upstream
        # release tag + source hash so the latency-histogram dependency is just
        # as reproducible as everything pulled from nixpkgs. Bump deliberately.
        hdrHistogram = pkgs.stdenv.mkDerivation rec {
          pname = "hdr_histogram_c";
          version = "0.11.8";
          src = pkgs.fetchFromGitHub {
            owner = "HdrHistogram";
            repo = "HdrHistogram_c";
            rev = version;
            hash = "sha256-TFlrC4bgK8o5KRZcLMlYU5EO9Oqaqe08PjJgmsUl51M=";
          };
          nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config ];
          buildInputs = [ pkgs.zlib ];
          # Build the static lib only; headers + pkg-config + CMake config are
          # what the bench target consumes.
          cmakeFlags = [
            "-DHDR_HISTOGRAM_BUILD_PROGRAMS=OFF"
            "-DHDR_HISTOGRAM_BUILD_SHARED=ON"
          ];
        };

        # Pinned C++20 toolchains. The project must compile under BOTH clang and
        # gcc (acceptance criterion). The devshell defaults to clang (stdenv);
        # gcc is on PATH as `g++` for the dual-compiler check.
        commonTools = [
          pkgs.cmake
          pkgs.ninja
          pkgs.pkg-config
          pkgs.gtest
          pkgs.gbenchmark
          hdrHistogram
          pkgs.zlib          # hdr_histogram.pc has `Requires: zlib`; expose it on
                             # the devshell pkg-config path so CMake resolves it
          pkgs.gcc           # gcc/g++ for the dual-compiler build
          pkgs.gdb
          pkgs.linuxPackages.perf
        ];
      in
      {
        packages.hdr_histogram_c = hdrHistogram;

        devShells.default = pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
          packages = commonTools;

          shellHook = ''
            echo "shimmy devshell — pinned C++20 toolchain"
            echo "  clang : $(clang --version | head -1)"
            echo "  gcc   : $(gcc --version | head -1)"
            echo "  cmake : $(cmake --version | head -1)"
            echo ""
            echo "Build:   cmake -S . -B build -G Ninja && cmake --build build"
            echo "Test:    ctest --test-dir build --output-on-failure"
            echo "Bench:   ./build/bench/shimmy_bench"
          '';
        };
      });
}
