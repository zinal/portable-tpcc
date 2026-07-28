pkgs: atrs: with pkgs; rec {
  version = "7.2.0";
  pname = "libpqxx";

  src = fetchFromGitHub {
    owner = "jtv";
    repo = pname;
    rev = version;
    sha256 = "04i3y9r13ldhvj8df50b5yb8754ykgj6f8m04sbd493jxnp91qbz";
  };

  nativeBuildInputs = [ python3 ];
}
