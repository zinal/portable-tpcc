pkgs: attrs: with pkgs; with attrs; rec {
  version = "18.4";
  version_with_underscores = "${lib.replaceStrings ["."] ["_"] version}";

  src = fetchFromGitHub {
    owner = "postgres";
    repo = "postgres";
    rev = "REL_${version_with_underscores}";
    hash = "sha256-Ac/Dqcj8vjcW3my5vsnKaMiQqTq/HPtUzckJ3SMyrfA=";
  };

  patches = [];

  nativeBuildInputs = [
    bison
    flex
    perl
    pkg-config
  ];

  buildPhase = ''
    make -j$NIX_BUILD_CORES submake-generated-headers
    make -j$NIX_BUILD_CORES -C src/common
    make -j$NIX_BUILD_CORES -C src/port
    make -j$NIX_BUILD_CORES -C src/interfaces/libpq all-shared-lib
  '';

  configureFlags = attrs.configureFlags ++ [
    "--without-gssapi"
    "--build=x86_64-unknown-linux-gnu"
  ];
}
