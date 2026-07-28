set -eux
sed -i '1s/^/#pragma clang system_header\n/' "googletest/include/gtest/gtest.h"
