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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
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
        out.set_stop_reason(status.stopReason);
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

    // Empty if the request is well formed.
    std::string toCommand(const pb::AdvanceRequest& request, Command& command)
    {
        command.kind = CommandKind::AdvanceTo;
        if (request.has_report_step()) {
            command.targetStep = request.report_step();
        } else if (request.has_date()) {
            command.date = request.date();
        } else {
            return "AdvanceTo needs a report step or a date";
        }
        for (const auto& stop : request.stop()) {
            if (stop.key().empty()
                || stop.comparison() == pb::StopCondition::COMPARISON_UNSPECIFIED) {
                return "A stop condition needs a key and a comparison";
            }
            command.stops.push_back({stop.key(),
                                     stop.comparison() == pb::StopCondition::COMPARISON_ABOVE,
                                     stop.value()});
        }
        return {};
    }

    // Names in deck records are quoted, so they cannot hold quotes, slashes or
    // spaces.
    bool validName(const std::string& name)
    {
        return !name.empty() && name.find_first_of("'/ \t\r\n") == std::string::npos;
    }

    // Writes the WSALT, WTRACER and WTEMP records for an InjectionCompositionRequest.
    // Empty error if the request is well formed.
    std::string compositionText(const pb::InjectionCompositionRequest& request, std::string& error)
    {
        const auto& well = request.well();
        if (!validName(well)) {
            error = "A well name is needed, without quotes, slashes or spaces";
            return {};
        }
        if (!request.has_salt_concentration() && !request.has_temperature()
            && request.tracers().empty()) {
            error = "Give a salt concentration, a temperature or at least one tracer";
            return {};
        }
        std::ostringstream text;
        text << std::setprecision(std::numeric_limits<double>::max_digits10);
        if (request.has_salt_concentration()) {
            const double salt = request.salt_concentration();
            if (!std::isfinite(salt) || salt < 0.0) {
                error = "Salt concentration must be finite and not negative";
                return {};
            }
            text << "WSALT\n '" << well << "' " << salt << " /\n/\n";
        }
        // Sorted, so that the same request always gives the same text.
        const std::map<std::string, double> tracers(request.tracers().begin(),
                                                    request.tracers().end());
        if (!tracers.empty()) {
            text << "WTRACER\n";
            for (const auto& [name, concentration] : tracers) {
                if (!validName(name)) {
                    error = "Tracer names cannot be empty or hold quotes, slashes or spaces";
                    return {};
                }
                if (!std::isfinite(concentration) || concentration < 0.0) {
                    error = "Tracer concentrations must be finite and not negative";
                    return {};
                }
                text << " '" << well << "' '" << name << "' " << concentration << " /\n";
            }
            text << "/\n";
        }
        if (request.has_temperature()) {
            if (!std::isfinite(request.temperature())) {
                error = "Temperature must be finite";
                return {};
            }
            text << "WTEMP\n '" << well << "' " << request.temperature() << " /\n/\n";
        }
        return text.str();
    }

    // One deck item: the value, or 1* when it is not given.
    std::string item(bool given, double value)
    {
        if (!given) {
            return "1*";
        }
        std::ostringstream out;
        out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        return out.str();
    }

    // Writes the WCONPROD or WCONINJE record for a WellControlRequest. Empty
    // error if the request is well formed.
    std::string wellControlText(const pb::WellControlRequest& request, std::string& error)
    {
        const auto& well = request.well();
        if (!validName(well)) {
            error = "A well name is needed, without quotes, slashes or spaces";
            return {};
        }
        const std::string status = request.status().empty() ? "OPEN" : request.status();
        if (status != "OPEN" && status != "STOP" && status != "SHUT") {
            error = "Status must be OPEN, STOP or SHUT";
            return {};
        }
        for (const double value : {request.oil_rate(),
                                   request.water_rate(),
                                   request.gas_rate(),
                                   request.liquid_rate(),
                                   request.reservoir_rate(),
                                   request.bhp(),
                                   request.thp(),
                                   request.surface_rate()}) {
            if (!std::isfinite(value)) {
                error = "Targets and limits must be finite";
                return {};
            }
        }

        const auto& control = request.control();
        std::ostringstream text;
        if (request.injector_type().empty()) {
            const std::map<std::string, bool> targets {
                {"ORAT", request.has_oil_rate()},
                {"WRAT", request.has_water_rate()},
                {"GRAT", request.has_gas_rate()},
                {"LRAT", request.has_liquid_rate()},
                {"RESV", request.has_reservoir_rate()},
                {"BHP", request.has_bhp()},
                {"THP", request.has_thp()},
            };
            const auto target = targets.find(control);
            if (target == targets.end()) {
                error = "Producer control must be ORAT, WRAT, GRAT, LRAT, RESV, BHP or THP";
                return {};
            }
            if (!target->second) {
                error = "Control " + control + " needs its target";
                return {};
            }
            text << "WCONPROD\n '" << well << "' '" << status << "' '" << control << "' "
                 << item(request.has_oil_rate(), request.oil_rate()) << ' '
                 << item(request.has_water_rate(), request.water_rate()) << ' '
                 << item(request.has_gas_rate(), request.gas_rate()) << ' '
                 << item(request.has_liquid_rate(), request.liquid_rate()) << ' '
                 << item(request.has_reservoir_rate(), request.reservoir_rate()) << ' '
                 << item(request.has_bhp(), request.bhp()) << ' '
                 << item(request.has_thp(), request.thp()) << " /\n/\n";
            return text.str();
        }

        const auto& type = request.injector_type();
        if (type != "WATER" && type != "GAS" && type != "OIL") {
            error = "Injector type must be WATER, GAS or OIL";
            return {};
        }
        const std::map<std::string, bool> targets {
            {"RATE", request.has_surface_rate()},
            {"RESV", request.has_reservoir_rate()},
            {"BHP", request.has_bhp()},
            {"THP", request.has_thp()},
        };
        const auto target = targets.find(control);
        if (target == targets.end()) {
            error = "Injector control must be RATE, RESV, BHP or THP";
            return {};
        }
        if (!target->second) {
            error = "Control " + control + " needs its target";
            return {};
        }
        text << "WCONINJE\n '" << well << "' '" << type << "' '" << status << "' '" << control
             << "' " << item(request.has_surface_rate(), request.surface_rate()) << ' '
             << item(request.has_reservoir_rate(), request.reservoir_rate()) << ' '
             << item(request.has_bhp(), request.bhp()) << ' '
             << item(request.has_thp(), request.thp()) << " /\n/\n";
        return text.str();
    }

    void fill(const ScheduleChange& change, pb::ApplyScheduleReply& out)
    {
        out.set_text(change.text);
        for (const auto& keyword : change.keywords) {
            out.add_keywords(keyword);
        }
        out.set_report_step(change.reportStep);
        for (const auto& well : change.wells) {
            out.add_wells(well);
        }
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
        auto credentials = grpc::InsecureServerCredentials();
        if (!options.tlsCertificate.empty()) {
            grpc::SslServerCredentialsOptions ssl;
            ssl.pem_key_cert_pairs.push_back({options.tlsKey, options.tlsCertificate});
            credentials = grpc::SslServerCredentials(ssl);
        }
        builder.AddListeningPort(options.endpoint, credentials, &port_);
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
            for (auto* command : {&pending_, &current_}) {
                if (*command) {
                    (*command)->progress->done = true;
                    (*command)->promise.set_value(ended);
                    command->reset();
                }
            }
        }
        progressCv_.notify_all();
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
        const bool woken
            = cv_.wait_for(lock, idleTimeout_, [this] { return pending_.has_value() || abort_; });
        if (!woken) {
            return std::nullopt;
        }
        if (abort_ && !pending_) {
            Command abort;
            abort.kind = CommandKind::Abort;
            abort.reason = abortReason_;
            return abort;
        }
        Command command = std::move(pending_->command);
        current_ = std::move(pending_);
        pending_.reset();
        if (!abort_) {
            cancel_ = false;
        }
        return command;
    }

    void reply(Reply reply)
    {
        std::optional<Pending> current;
        {
            std::lock_guard lock(mutex_);
            current.swap(current_);
            if (current) {
                current->progress->done = true;
            }
        }
        progressCv_.notify_all();
        if (current) {
            current->promise.set_value(std::move(reply));
        }
    }

    void publishStatus(const Status& status)
    {
        std::lock_guard lock(mutex_);
        status_ = status;
    }

    void progress(const Status& status)
    {
        {
            std::lock_guard lock(mutex_);
            if (current_) {
                current_->progress->statuses.push_back(status);
            }
        }
        progressCv_.notify_all();
    }

    bool cancelRequested() const
    {
        return cancel_;
    }

    bool abortRequested() const
    {
        return abort_;
    }

    std::string abortReason() const
    {
        std::lock_guard lock(mutex_);
        return abortReason_;
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

    grpc::Status AdvanceTo(grpc::ServerContext* context,
                           const pb::AdvanceRequest* request,
                           grpc::ServerWriter<pb::StatusReply>* writer) override
    {
        Command command;
        if (const auto error = toCommand(*request, command); !error.empty()) {
            return {grpc::StatusCode::INVALID_ARGUMENT, error};
        }
        std::future<Reply> future;
        std::shared_ptr<Progress> progress;
        if (const auto refused = enqueue(*context, std::move(command), future, progress)) {
            return toGrpc(*refused);
        }

        // A client that goes away cancels the advance.
        bool streaming = true;
        for (bool done = false; !done;) {
            std::deque<Status> statuses;
            {
                std::unique_lock lock(mutex_);
                progressCv_.wait(
                    lock, [&progress] { return !progress->statuses.empty() || progress->done; });
                statuses.swap(progress->statuses);
                done = progress->done;
            }
            for (const auto& status : statuses) {
                pb::StatusReply message;
                fill(status, message);
                if (streaming && !writer->Write(message)) {
                    streaming = false;
                    cancel_ = true;
                }
            }
        }

        // The last status sent already carries the stop reason.
        return toGrpc(future.get());
    }

    grpc::Status
    Cancel(grpc::ServerContext* context, const pb::CancelRequest*, pb::CancelReply*) override
    {
        if (!authorised(*context)) {
            return {grpc::StatusCode::UNAUTHENTICATED, "Missing or wrong token"};
        }
        cancel_ = true;
        return grpc::Status::OK;
    }

    grpc::Status
    Abort(grpc::ServerContext* context, const pb::AbortRequest* request, pb::AbortReply*) override
    {
        if (!authorised(*context)) {
            return {grpc::StatusCode::UNAUTHENTICATED, "Missing or wrong token"};
        }
        {
            std::lock_guard lock(mutex_);
            abortReason_ = request->reason();
            abort_ = true;
            cancel_ = true;
        }
        cv_.notify_one();
        return grpc::Status::OK;
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

    grpc::Status ApplySchedule(grpc::ServerContext* context,
                               const pb::ApplyScheduleRequest* request,
                               pb::ApplyScheduleReply* out) override
    {
        return applySchedule(*context, request->text(), *out);
    }

    grpc::Status SetInjectionComposition(grpc::ServerContext* context,
                                         const pb::InjectionCompositionRequest* request,
                                         pb::ApplyScheduleReply* out) override
    {
        std::string error;
        const std::string text = compositionText(*request, error);
        if (!error.empty()) {
            return {grpc::StatusCode::INVALID_ARGUMENT, error};
        }
        return applySchedule(*context, text, *out);
    }

    grpc::Status SetWellControl(grpc::ServerContext* context,
                                const pb::WellControlRequest* request,
                                pb::ApplyScheduleReply* out) override
    {
        std::string error;
        const std::string text = wellControlText(*request, error);
        if (!error.empty()) {
            return {grpc::StatusCode::INVALID_ARGUMENT, error};
        }
        return applySchedule(*context, text, *out);
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
    // Statuses sent while a command runs, for AdvanceTo.
    struct Progress {
        std::deque<Status> statuses;
        bool done = false;
    };

    struct Pending {
        Command command;
        std::promise<Reply> promise;
        std::shared_ptr<Progress> progress;
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

    // Queues the command for the simulator thread. Returns the reason if it
    // is refused.
    std::optional<Reply> enqueue(const grpc::ServerContext& context,
                                 Command command,
                                 std::future<Reply>& future,
                                 std::shared_ptr<Progress>& progress)
    {
        if (!authorised(context)) {
            return failure(ErrorCode::Unauthenticated, "Missing or wrong token");
        }
        {
            std::lock_guard lock(mutex_);
            if (closed_ || abort_) {
                return failure(ErrorCode::InvalidState, "Simulation has ended");
            }
            if (pending_ || current_) {
                return failure(ErrorCode::Busy, "Another command is in progress");
            }
            progress = std::make_shared<Progress>();
            pending_.emplace(Pending {std::move(command), {}, progress});
            future = pending_->promise.get_future();
        }
        cv_.notify_one();
        return std::nullopt;
    }

    grpc::Status applySchedule(const grpc::ServerContext& context,
                               const std::string& text,
                               pb::ApplyScheduleReply& out)
    {
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
            return {grpc::StatusCode::INVALID_ARGUMENT, "No keywords given"};
        }
        Command command;
        command.kind = CommandKind::ApplySchedule;
        command.text = text;
        const Reply reply = submit(&context, std::move(command));
        fill(reply.schedule, out);
        return toGrpc(reply);
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
        std::future<Reply> future;
        std::shared_ptr<Progress> progress;
        if (auto refused = enqueue(*context, std::move(command), future, progress)) {
            return std::move(*refused);
        }
        return future.get();
    }

    std::string token_;
    std::chrono::seconds idleTimeout_;
    int port_ = 0;
    std::unique_ptr<grpc::Server> server_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable progressCv_;
    std::optional<Pending> pending_;
    std::optional<Pending> current_;
    Status status_;
    bool closed_ = false;
    std::atomic<bool> cancel_ {false};
    std::atomic<bool> abort_ {false};
    std::string abortReason_;
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

void
ControlServer::progress(const Status& status)
{
    impl_->progress(status);
}

bool
ControlServer::cancelRequested() const
{
    return impl_->cancelRequested();
}

bool
ControlServer::abortRequested() const
{
    return impl_->abortRequested();
}

std::string
ControlServer::abortReason() const
{
    return impl_->abortReason();
}

} // namespace Opm::Control
