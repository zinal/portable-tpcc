#pragma once

#include "runner_display_data.h"
#include <log_backend.h>
#include "tui_base.h"

#include <ftxui/dom/elements.hpp>

namespace NTpcc {

class TRunnerTui : public TuiBase {
public:
    TRunnerTui(TLogBackendWithCapture& logBackend, std::shared_ptr<TRunDisplayData> data);

    void Update(std::shared_ptr<TRunDisplayData> data);

private:
    ftxui::Element BuildPreviewPart();
    ftxui::Element BuildThreadStatsPart();

    ftxui::Component BuildComponent() override;

private:
    TLogBackendWithCapture& LogBackend;
    std::shared_ptr<TRunDisplayData> DataToDisplay;
};

} // namespace NTpcc
