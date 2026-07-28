from devtools.yamaker.modules import GLOBAL
from devtools.yamaker.project import CMakeNinjaNixProject


def post_install(self):
    with self.yamakes["googletest"] as gtest:
        # re2 is used in custom gtest-port.h
        gtest.PEERDIR.add("contrib/libs/re2")

        # Fix windows compilation by removing
        # -DGTEST_HAS_PTHREAD=1 and
        # -DGTEST_CREATE_SHARED_LIBRARY=1
        #
        # Disable Posix RE (#include <regex.h>) support by providing
        # -DGTEST_HAS_POSIX_RE=0
        gtest.CFLAGS = [
            GLOBAL("-DGTEST_HAS_POSIX_RE=0"),
            GLOBAL("-DGTEST_HAS_STD_WSTRING=1"),
            GLOBAL("-DGTEST_USES_RE2=1"),
        ]

    with self.yamakes["googlemock"] as gmock:
        gmock.CFLAGS = []


gtest = CMakeNinjaNixProject(
    owners=["g:cpp-contrib"],
    arcdir="contrib/restricted/googletest",
    nixattr="gtest",
    disable_includes=[
        "acxx_demangle.h",
        "absl/",
        "devctl.h",
        "lib/fdio/",
        "lib/zx/",
        "mem.h",
        "qurt_event.h",
        "regex.h",
        "zircon/",
    ],
    copy_sources=[
        "docs/*.md",
    ],
    install_targets=["gmock", "gtest"],
    put={
        "gmock": "googlemock",
        "gtest": "googletest",
    },
    addincl_global={
        "googlemock": ["./include"],
        "googletest": ["./include"],
    },
    post_install=post_install,
)
