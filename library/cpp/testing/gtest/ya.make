LIBRARY()

PROVIDES(test_framework)

SUBSCRIBER(
    bulatman
    prettyboy
    thegeorg
    g:cpp-contrib
)

SRCS(
    gtest.cpp
    main.cpp
    matchers.cpp
)

PEERDIR(
    contrib/restricted/googletest/googlemock
    contrib/restricted/googletest/googletest
    library/cpp/string_utils/relaxed_escaper
    library/cpp/testing/common
    library/cpp/testing/gtest_extensions
    library/cpp/testing/hook
    library/cpp/json
)

END()

RECURSE_FOR_TESTS(ut)
