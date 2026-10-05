#include "check.h"

#include "run_config.h"

#include <artifacts.h>
#include <catalog.h>
#include <log.h>
#include <report.h>

#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace NTpcc {

TCheckReport RunDummyChecks(const TCheckRequest& request) {
    TCheckReport report;
    report.RunId = request.RunId;
    report.Instance = request.Instance;
    report.Phase = request.Phase == ECheckPhase::AfterImport ? "after-import" : "after-test";
    report.WarehouseCount = request.WarehouseCount;
    report.ProgressTotal = CountCatalogChecks(request.Phase);

    for (const auto& entry : CheckCatalog()) {
        if (!CheckAppliesToPhase(entry.Phase, request.Phase)) {
            continue;
        }
        RecordCheckResult(report, std::string(entry.Id), ECheckStatus::Passed);
    }
    if (report.Ok()) {
        std::cout << "Everything is good!" << std::endl;
    }
    return report;
}

void CheckSync(int warehouseCount, bool afterImport, int checkConcurrency) {
    TCheckRequest req;
    req.WarehouseCount = warehouseCount;
    req.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterTest;
    req.CheckConcurrency = checkConcurrency <= 1 ? 1 : checkConcurrency;
    auto report = RunDummyChecks(req);
    if (!report.Ok()) {
        throw std::runtime_error("dummy checks failed");
    }
}

TCheckReport TDummyCheckAdapter::Run(const TCheckRequest& request) {
    return RunDummyChecks(request);
}

int RunCheckFromRunConfig(const std::string& runConfigPath, const std::string& instance,
                          bool afterImport, bool afterRun, int checkConcurrency) {
    if (afterImport == afterRun) {
        throw std::runtime_error("check requires exactly one of --after-import or --after-test");
    }

    const auto doc = LoadRunConfigDocument(runConfigPath);
    const std::string instanceDir = InstanceWorkDir(doc, "check", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();
    WriteProcessJson(paths, doc, instance, "check", static_cast<int>(::getpid()), nonce);

    int exitCode = 1;
    try {
        TCheckRequest req;
        req.WarehouseCount = doc.ScaleWarehouses;
        req.Phase = afterImport ? ECheckPhase::AfterImport : ECheckPhase::AfterTest;
        req.Path = doc.Path;
        req.RunId = doc.RunId;
        req.Instance = instance;
        req.CheckConcurrency = checkConcurrency <= 1 ? 1 : checkConcurrency;

        TDummyCheckAdapter adapter;
        const auto report = adapter.Run(req);

        const std::string checksDir = doc.RunDir + "/checks";
        const std::string reportPath = checksDir + "/" + report.Phase + ".json";
        WriteCheckReportJson(reportPath, report);
        LOG_I("Check report written to " << reportPath);
        exitCode = report.Ok() ? 0 : 1;
    } catch (const std::exception& ex) {
        LOG_E("Check failed: " << ex.what());
        exitCode = 1;
    }

    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

} // namespace NTpcc
