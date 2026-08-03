/* Platform dispatcher for OceanBase Connector/C (obconnector-c). */
#pragma once

#if defined(_MSC_VER)
#   error "obconnector-c ya.make packaging currently targets Unix-like hosts"
#elif defined(__APPLE__)
#   include "ma_config-linux.h"
#else
#   include "ma_config-linux.h"
#endif
