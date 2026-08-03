#include "logs_scroller.h"

#include "scroller.h"

#include <log.h>

#include <util/string/builder.h>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <optional>

using namespace ftxui;

namespace NTpcc {

namespace {

inline std::optional<Color::Palette16> GetFtxuiLogColor(ELogPriority priority) {
    switch (priority) {
        case TLOG_EMERG:
            [[fallthrough]];
        case TLOG_ALERT:
            [[fallthrough]];
        case TLOG_CRIT:
            [[fallthrough]];
        case TLOG_ERR:
            return Color::Red;
        case TLOG_WARNING:
            return Color::Yellow;
        case TLOG_NOTICE:
            [[fallthrough]];
        case TLOG_INFO:
            [[fallthrough]];
        case TLOG_DEBUG:
            [[fallthrough]];
        case TLOG_RESOURCES:
            [[fallthrough]];
        default:
            return std::nullopt;
    }
}

} // namespace

Component LogsScroller(TLogBackendWithCapture& logBackend) {
    return Scroller(Renderer([&] {
        Elements logElements;

        logBackend.GetLogLines([&](ELogPriority priority, const std::string& line) {
            const size_t dtLen = GetLenOfFormatDate8601Part();

            TString withoutColor;
            if (line.size() >= dtLen) {
                size_t searchFrom = dtLen;
                size_t colonPos = line.find(": ", searchFrom);
                if (colonPos != std::string::npos) {
                    withoutColor = TStringBuilder() << line.substr(0, dtLen - 1) << " "
                        << PriorityToString(priority) << line.substr(colonPos);
                }
            }

            if (withoutColor.empty()) {
                withoutColor = TString(line);
            }

            while (!withoutColor.empty() && (withoutColor.back() == '\n' || withoutColor.back() == '\r')) {
                withoutColor.pop_back();
            }

            auto colorOpt = GetFtxuiLogColor(priority);
            logElements.push_back(paragraph(withoutColor)
                | (colorOpt ? color(*colorOpt) : color(Color::Default)));
        });

        auto logsContent = vbox(logElements) | flex;
        return logsContent;
    }), "Logs");
}

} // namespace NTpcc
