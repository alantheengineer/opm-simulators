/*
  Copyright 2026 Dr Alan Tominey

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <config.h>

#include <opm/simulators/flow/control/ControlServer.hpp>

#include <opm/simulators/flow/control/opm_control.grpc.pb.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Opm::Control
{

namespace
{

    namespace pb = opm::control::v1;

    // Values per chunk. 2 MB keeps each message under the 4 MB default receive
    // limit of gRPC clients.
    constexpr std::size_t chunkValues = 262144;

    bool isLoopback(std::string_view endpoint)
    {
        return endpoint.starts_with("127.0.0.1:") || endpoint.starts_with("localhost:")
            || endpoint.starts_with("[::1]:");
    }

    Reply failure(ErrorCode code, std::string message)
    {
        Reply reply;
        reply.code = code;
        reply.message = std::move(message);
        return reply;
    }

    grpc::Status toGrpc(const Reply& reply)
    {
        switch (reply.code) {
        case ErrorCode::None:
            return grpc::Status::OK;
        case ErrorCode::InvalidArgument:
            return {grpc::StatusCode::INVALID_ARGUMENT, reply.message};
        case ErrorCode::InvalidState:
            return {grpc::StatusCode::FAILED_PRECONDITION, reply.message};
        case ErrorCode::Busy:
            return {grpc::StatusCode::UNAVAILABLE, reply.message};
        case ErrorCode::Unauthenticated:
            return {grpc::StatusCode::UNAUTHENTICATED, reply.message};
        case ErrorCode::Internal:
            break;
        }
        return {grpc::StatusCode::INTERNAL, reply.message};
    }

    pb::State toProto(State state)
    {
        switch (state) {
        case State::Ready:
            return pb::STATE_READY;
        case State::Stepping:
            return pb::STATE_STEPPING;
        case State::Done:
            return pb::STATE_DONE;
        case State::Finished:
            return pb::STATE_FINISHED;
        case State::Failed:
            return pb::STATE_FAILED;
        }
        return pb::STATE_UNSPECIFIED;
    }

    void fill(const Status& status, pb::StatusReply& out)
    {
        out.set_state(toProto(status.state));
        out.set_report_step(status.reportStep);
        out.set_report_steps_total(status.reportStepsTotal);
        out.set_elapsed_seconds(status.elapsedSeconds);
        out.set_last_step_wall_seconds(status.lastStepWallSeconds);
        out.set_error(status.error);
        auto& step = *out.mutable_last_step();
        step.set_time_steps(status.lastStep.timeSteps);
        step.set_cuts(status.lastStep.cuts);
        step.set_newton_iterations(status.lastStep.newtonIterations);
        step.set_linear_iterations(status.lastStep.linearIterations);
        step.set_wasted_newton_iterations(status.lastStep.wastedNewtonIterations);
    }

    void fill(const Info& info, pb::InfoReply& out)
    {
        out.set_engine_version(info.engineVersion);
        out.set_ranks(info.ranks);
        out.set_threads_per_rank(info.threadsPerRank);
        out.mutable_grid_dims()->Add(info.gridDims.begin(), info.gridDims.end());
        out.set_active_cells(info.activeCells);
        for (const auto& date : info.reportDates) {
            out.add_report_dates(date);
        }
        out.set_unit_system(info.unitSystem);
    }

    void fill(const WellData& data, pb::WellDataReply& out)
    {
        for (const auto& well : data.wells) {
            auto& w = *out.add_wells();
            w.set_name(well.name);
            w.set_status(well.status);
            w.set_injector(well.injector);
            w.set_control(well.control);
            w.set_oil_rate(well.oilRate);
            w.set_water_rate(well.waterRate);
            w.set_gas_rate(well.gasRate);
            w.set_bhp(well.bhp);
            w.set_thp(well.thp);
            w.set_temperature(well.temperature);
            w.set_salt_rate(well.saltRate);
        }
        out.set_liquid_rate_unit(data.liquidRateUnit);
        out.set_gas_rate_unit(data.gasRateUnit);
        out.set_pressure_unit(data.pressureUnit);
        out.set_temperature_unit(data.temperatureUnit);
        out.set_salt_rate_unit(data.saltRateUnit);
    }

    void fill(const SummaryData& data, pb::SummaryReply& out)
    {
        out.mutable_report_steps()->Add(data.reportSteps.begin(), data.reportSteps.end());
        out.mutable_elapsed_seconds()->Add(data.elapsedSeconds.begin(), data.elapsedSeconds.end());
        for (const auto& series : data.series) {
            auto& s = *out.add_series();
            s.set_key(series.key);
            s.mutable_values()->Add(series.values.begin(), series.values.end());
        }
    }

    Command toCommand(const pb::CellDataRequest& request)
    {
        Command command;
        command.kind = CommandKind::GetCellData;
        command.fields.assign(request.fields().begin(), request.fields().end());
        command.siUnits = request.si_units();
        if (request.has_indices()) {
            command.cells.mode = CellSelection::Mode::Indices;
            command.cells.indices.assign(request.indices().active().begin(),
                                         request.indices().active().end());
        } else if (request.has_box()) {
            const auto& box = request.box();
            command.cells.mode = CellSelection::Mode::Box;
            command.cells.box = {box.i1(), box.i2(), box.j1(), box.j2(), box.k1(), box.k2()};
        }
        return command;
    }

    // Sends each field in chunks. False if the client went away.
    bool writeChunks(const CellData& data, grpc::ServerWriter<pb::CellDataChunk>& writer)
    {
        bool first = true;
        for (const auto& field : data.fields) {
            const std::size_t total = field.values.size();
            std::size_t offset = 0;
            do {
                const std::size_t count = std::min(chunkValues, total - offset);
                const auto begin = field.values.begin() + static_cast<std::ptrdiff_t>(offset);
                pb::CellDataChunk chunk;
                chunk.set_field(field.name);
                chunk.set_unit(field.unit);
                chunk.set_offset(static_cast<std::int64_t>(offset));
                chunk.set_total(static_cast<std::int64_t>(total));
                chunk.mutable_values()->Add(begin, begin + static_cast<std::ptrdiff_t>(count));
                if (first) {
                    chunk.mutable_cells()->Add(data.cells.begin(), data.cells.end());
                    first = false;
                }
                if (!writer.Write(chunk)) {
                    return false;
                }
                offset += count;
            } while (offset < total);
        }
        return true;
    }

} // namespace

class ControlServer::Impl final : public pb::Control::Service
{
public:
    explicit Impl(const Options& options)
        : token_(options.token)
        , idleTimeout_(options.idleTimeout)
    {
        if (token_.empty() && !isLoopback(options.endpoint)) {
            throw std::invalid_argument("Control endpoint " + options.endpoint
                                        + " is not a loopback address and needs a token file");
        }

        grpc::ResourceQuota quota("opm-control");
        quota.SetMaxThreads(4);

        grpc::ServerBuilder builder;
        builder.SetResourceQuota(quota);
        builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS, 1);
        builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, 1);
        builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, 2);
        builder.AddListeningPort(options.endpoint, grpc::InsecureServerCredentials(), &port_);
        builder.RegisterService(this);
        server_ = builder.BuildAndStart();
        if (!server_ || port_ == 0) {
            throw std::runtime_error("Cannot listen on control endpoint " + options.endpoint);
        }
    }

    // Releases any handler still waiting so that Shutdown() can return.
    ~Impl() override
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            const Reply ended = failure(ErrorCode::InvalidState, "Simulation has ended");
            if (pending_) {
                pending_->promise.set_value(ended);
                pending_.reset();
            }
            if (current_) {
                current_->set_value(ended);
                current_.reset();
            }
        }
        server_->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    int port() const
    {
        return port_;
    }

    std::optional<Command> waitForCommand()
    {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, idleTimeout_, [this] { return pending_.has_value(); })) {
            return std::nullopt;
        }
        Command command = std::move(pending_->command);
        current_ = std::move(pending_->promise);
        pending_.reset();
        return command;
    }

    void reply(Reply reply)
    {
        std::optional<std::promise<Reply>> promise;
        {
            std::lock_guard lock(mutex_);
            promise.swap(current_);
        }
        if (promise) {
            promise->set_value(std::move(reply));
        }
    }

    void publishStatus(const Status& status)
    {
        std::lock_guard lock(mutex_);
        status_ = status;
    }

    grpc::Status
    GetInfo(grpc::ServerContext* context, const pb::GetInfoRequest*, pb::InfoReply* out) override
    {
        const Reply reply = submit(context, CommandKind::GetInfo);
        fill(reply.info, *out);
        return toGrpc(reply);
    }

    grpc::Status GetStatus(grpc::ServerContext* context,
                           const pb::GetStatusRequest*,
                           pb::StatusReply* out) override
    {
        if (!authorised(*context)) {
            return {grpc::StatusCode::UNAUTHENTICATED, "Missing or wrong token"};
        }
        std::lock_guard lock(mutex_);
        fill(status_, *out);
        return grpc::Status::OK;
    }

    grpc::Status
    Step(grpc::ServerContext* context, const pb::StepRequest*, pb::StatusReply* out) override
    {
        const Reply reply = submit(context, CommandKind::Step);
        fill(reply.status, *out);
        return toGrpc(reply);
    }

    grpc::Status
    Finish(grpc::ServerContext* context, const pb::FinishRequest*, pb::StatusReply* out) override
    {
        const Reply reply = submit(context, CommandKind::Finish);
        fill(reply.status, *out);
        return toGrpc(reply);
    }

    grpc::Status
    Shutdown(grpc::ServerContext* context, const pb::ShutdownRequest*, pb::ShutdownReply*) override
    {
        return toGrpc(submit(context, CommandKind::Shutdown));
    }

    grpc::Status GetCellData(grpc::ServerContext* context,
                             const pb::CellDataRequest* request,
                             grpc::ServerWriter<pb::CellDataChunk>* writer) override
    {
        if (request->fields().empty()) {
            return {grpc::StatusCode::INVALID_ARGUMENT, "No fields requested"};
        }
        const Reply reply = submit(context, toCommand(*request));
        if (reply.code != ErrorCode::None) {
            return toGrpc(reply);
        }
        if (!writeChunks(reply.cellData, *writer)) {
            return {grpc::StatusCode::CANCELLED, "Client closed the stream"};
        }
        return grpc::Status::OK;
    }

    grpc::Status GetWellData(grpc::ServerContext* context,
                             const pb::WellDataRequest* request,
                             pb::WellDataReply* out) override
    {
        Command command;
        command.kind = CommandKind::GetWellData;
        command.names.assign(request->wells().begin(), request->wells().end());
        const Reply reply = submit(context, std::move(command));
        fill(reply.wellData, *out);
        return toGrpc(reply);
    }

    grpc::Status GetSummary(grpc::ServerContext* context,
                            const pb::SummaryRequest* request,
                            pb::SummaryReply* out) override
    {
        Command command;
        command.kind = CommandKind::GetSummary;
        command.names.assign(request->keys().begin(), request->keys().end());
        command.history = request->history();
        const Reply reply = submit(context, std::move(command));
        fill(reply.summary, *out);
        return toGrpc(reply);
    }

private:
    struct Pending {
        Command command;
        std::promise<Reply> promise;
    };

    bool authorised(const grpc::ServerContext& context) const
    {
        if (token_.empty()) {
            return true;
        }
        const auto& metadata = context.client_metadata();
        const auto it = metadata.find("authorization");
        return it != metadata.end()
            && std::string_view(it->second.data(), it->second.size()) == "Bearer " + token_;
    }

    Reply submit(const grpc::ServerContext* context, CommandKind kind)
    {
        Command command;
        command.kind = kind;
        return submit(context, std::move(command));
    }

    // Hands the command to the simulator thread and waits for its reply.
    Reply submit(const grpc::ServerContext* context, Command command)
    {
        if (!authorised(*context)) {
            return failure(ErrorCode::Unauthenticated, "Missing or wrong token");
        }
        std::future<Reply> future;
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return failure(ErrorCode::InvalidState, "Simulation has ended");
            }
            if (pending_ || current_) {
                return failure(ErrorCode::Busy, "Another command is in progress");
            }
            pending_.emplace(Pending {std::move(command), {}});
            future = pending_->promise.get_future();
        }
        cv_.notify_one();
        return future.get();
    }

    std::string token_;
    std::chrono::seconds idleTimeout_;
    int port_ = 0;
    std::unique_ptr<grpc::Server> server_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Pending> pending_;
    std::optional<std::promise<Reply>> current_;
    Status status_;
    bool closed_ = false;
};

ControlServer::ControlServer(const Options& options)
    : impl_(std::make_unique<Impl>(options))
{
}

ControlServer::~ControlServer() = default;

int
ControlServer::port() const
{
    return impl_->port();
}

std::optional<Command>
ControlServer::waitForCommand()
{
    return impl_->waitForCommand();
}

void
ControlServer::reply(Reply reply)
{
    impl_->reply(std::move(reply));
}

void
ControlServer::publishStatus(const Status& status)
{
    impl_->publishStatus(status);
}

} // namespace Opm::Control
