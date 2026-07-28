from devtools.yamaker.modules import GLOBAL
from devtools.yamaker.project import CMakeNinjaNixProject


def post_install(self):
    with self.yamakes["."] as spdlog:
        spdlog.CFLAGS.remove("-DSPDLOG_FMT_EXTERNAL")
        spdlog.CFLAGS.append(GLOBAL("-DSPDLOG_FMT_EXTERNAL"))
        spdlog.CFLAGS.sort()

        spdlog.CFLAGS.remove("-DSPDLOG_FWRITE_UNLOCKED")
        spdlog.after(
            "CFLAGS",
            """
            IF (OS_LINUX OR OS_WINDOWS)
                # NB:
                # On Windows _fwrite_nolock() will be used
                # On Android these are available since API_LEVEL 28 (Android P)
                CFLAGS(
                    -DSPDLOG_FWRITE_UNLOCKED
                )
            ENDIF()
            """,
        )


spdlog = CMakeNinjaNixProject(
    nixattr="spdlog",
    arcdir="contrib/restricted/spdlog",
    owners=["g:cpp-contrib"],
    disable_includes=[
        "format",
        "spdlog/fmt/bundled/*",
    ],
    copy_sources=[
        "include/**/*.h",
    ],
    copy_sources_except=[
        "include/spdlog/fmt/bundled/*.h",
    ],
    post_install=post_install,
)
