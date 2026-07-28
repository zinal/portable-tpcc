from devtools.yamaker import fileutil
from devtools.yamaker.project import GNUMakeNixProject


def post_install(self):
    fileutil.re_sub_dir(
        self.dstdir,
        "#include <experimental/optional>",
        "#error #include <experimental/optional>",
    )
    self.yamakes["."].SRCS.add("src/prepared_statement.cxx")
    self.install_yamakes()


libpqxx = GNUMakeNixProject(
    owners=["g:cpp-contrib"],
    arcdir="contrib/libs/libpqxx",
    nixattr="libpqxx",
    makeflags=["-C", "src", "libpqxx.la"],
    copy_sources={
        "include/**",
    },
    copy_sources_except={
        "include/Makefile",
        "include/CMakeLists.txt",
        "include/CMakeLists.txt.template",
        "include/**/Makefile",
        "include/**/*.template",
        "include/pqxx/config-internal-autotools.h",
        "include/pqxx/config.h",
        "include/pqxx/doc/**",
        "include/pqxx/stamp-h1",
    },
    addincl_global={".": {"./include"}},
    post_install=post_install,
)
