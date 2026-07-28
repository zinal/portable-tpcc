self: super: with self; rec {
  version = "1.17.0";

  src = fetchFromGitHub {
    owner  = "gabime";
    repo   = "spdlog";
    rev    = "v${version}";
    hash = "sha256-bL3hQmERXNwGmDoi7+wLv/TkppGhG6cO47k1iZvJGzY=";
  };

  cmakeFlags = [
    "-DSPDLOG_BUILD_SHARED=ON"
    "-DSPDLOG_BUILD_EXAMPLE=OFF"
    "-DSPDLOG_BUILD_BENCH=OFF"
    "-DSPDLOG_BUILD_TESTS=OFF"
    "-DSPDLOG_FMT_EXTERNAL=ON"
    "-DSPDLOG_PREVENT_CHILD_FD=ON"
  ];

  # override outdated patch from nixpkgs:
  # https://github.com/NixOS/nixpkgs/blob/9b49161f7f49d556003f895c6a4e78d6b69b5384/pkgs/development/libraries/spdlog/default.nix#L19
  patches = [];
}
