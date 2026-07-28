#pragma once

#include <log_backend.h>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

namespace NTpcc {

ftxui::Component LogsScroller(TLogCapture& logCapture);

} // namespace NTpcc
