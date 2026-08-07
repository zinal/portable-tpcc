#include "arrow_upsert.h"

#include <contrib/libs/apache/arrow/cpp/src/arrow/io/memory.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/ipc/writer.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <cstring>
#include <stdexcept>

namespace NTpcc {

namespace {

void ThrowArrow(const arrow::Status& status, const char* what) {
    if (!status.ok()) {
        throw std::runtime_error(std::string(what) + ": " + status.ToString());
    }
}

class TFixedStringOutputStream final : public arrow::io::OutputStream {
public:
    explicit TFixedStringOutputStream(std::string* out)
        : Out_(out)
        , Position_(0)
    {}

    arrow::Status Close() override {
        Out_ = nullptr;
        return arrow::Status::OK();
    }

    bool closed() const override {
        return Out_ == nullptr;
    }

    arrow::Result<int64_t> Tell() const override {
        return Position_;
    }

    arrow::Status Write(const void* data, int64_t nbytes) override {
        if (nbytes > 0) {
            if (!Out_ || Out_->size() - static_cast<size_t>(Position_) < static_cast<size_t>(nbytes)) {
                return arrow::Status::IOError("Arrow output buffer is too small");
            }
            std::memcpy(Out_->data() + Position_, data, static_cast<size_t>(nbytes));
            Position_ += nbytes;
        }
        return arrow::Status::OK();
    }

    int64_t GetPosition() const {
        return Position_;
    }

private:
    std::string* Out_;
    int64_t Position_;
};

} // anonymous

std::string SerializeArrowSchema(const arrow::Schema& schema) {
    auto buffer = arrow::ipc::SerializeSchema(schema);
    ThrowArrow(buffer.status(), "serialize Arrow schema");
    return std::string(
        reinterpret_cast<const char*>((*buffer)->data()),
        static_cast<size_t>((*buffer)->size()));
}

std::string SerializeArrowBatch(const std::shared_ptr<arrow::RecordBatch>& batch) {
    auto writeOptions = arrow::ipc::IpcWriteOptions::Defaults();
    writeOptions.use_threads = false;

    arrow::ipc::IpcPayload payload;
    ThrowArrow(
        arrow::ipc::GetRecordBatchPayload(*batch, writeOptions, &payload),
        "build Arrow IPC payload");

    int32_t metadataLength = 0;
    arrow::io::MockOutputStream mock;
    ThrowArrow(
        arrow::ipc::WriteIpcPayload(payload, writeOptions, &mock, &metadataLength),
        "measure Arrow IPC payload");

    std::string out;
    out.resize(static_cast<size_t>(mock.GetExtentBytesWritten()));
    TFixedStringOutputStream stream(&out);
    ThrowArrow(
        arrow::ipc::WriteIpcPayload(payload, writeOptions, &stream, &metadataLength),
        "write Arrow IPC payload");
    if (stream.GetPosition() != static_cast<int64_t>(out.size())) {
        throw std::runtime_error("Arrow IPC payload size mismatch");
    }
    return out;
}

void BulkUpsertArrow(
    TYdbConnection& connection,
    const std::string& table,
    const std::shared_ptr<arrow::RecordBatch>& batch)
{
    if (!batch || batch->num_rows() == 0) {
        return;
    }
    const std::string schema = SerializeArrowSchema(*batch->schema());
    const std::string data = SerializeArrowBatch(batch);
    auto status = connection.TableClient().BulkUpsert(
        connection.TablePath(table),
        NYdb::NTable::EDataFormat::ApacheArrow,
        data,
        schema).GetValueSync();
    if (!status.IsSuccess()) {
        throw std::runtime_error(
            "bulk upsert " + table + ": " + status.GetIssues().ToOneLineString());
    }
}

} // namespace NTpcc
