pkgs: attrs: with pkgs; with attrs; rec {
  version = "1.17.0";

  src = fetchFromGitHub {
    owner = "google";
    repo = "googletest";
    rev = "v${version}";
    hash = "sha256-HIHMxAUR4bjmFLoltJeIAVSulVQ6kVuIT2Ku+lwAx/4=";
  };

  cmakeFlags = ["-DBUILD_SHARED_LIBS=1"];
  buildInputs = [];
  patches = [];
  postPatch = "";
}
