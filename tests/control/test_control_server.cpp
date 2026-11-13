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

#define BOOST_TEST_MODULE ControlServerTest
#include <boost/test/unit_test.hpp>

#include <opm/simulators/flow/control/ControlLoop.hpp>
#include <opm/simulators/flow/control/ControlServer.hpp>
#include <opm/simulators/flow/control/opm_control.grpc.pb.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace pb = opm::control::v1;
using namespace Opm::Control;

struct SerialComm {
    int rank() const
    {
        return 0;
    }
    template <class T>
    int broadcast(T*, int, int) const
    {
        return 0;
    }
    int max(int value) const
    {
        return value;
    }
};

struct FakeDriver {
    int steps = 3;
    int current = 0;
    int failAt = -1;
    bool finished = false;

    Info info() const
    {
        Info info;
        info.engineVersion = "test";
        info.ranks = 1;
        info.gridDims = {10, 10, 3};
        info.activeCells = 300;
        info.reportDates = {"2026-01-01T00:00:00Z", "2026-02-01T00:00:00Z"};
        info.unitSystem = "METRIC";
        return info;
    }
    int reportStep() const
    {
        return current;
    }
    int reportStepsTotal() const
    {
        return steps;
    }
    double elapsedSeconds() const
    {
        return 86400.0 * current;
    }
    bool done() const
    {
        return current >= steps;
    }
    void step()
    {
        if (current == failAt) {
            throw std::runtime_error("Linear solver failed");
        }
        std::this_thread::sleep_for(stepTime);
        ++current;
    }
    int finish()
    {
        finished = true;
        return EXIT_SUCCESS;
    }
    StepReport lastStep() const
    {
        StepReport report;
        report.timeSteps = 2;
        report.cuts = 1;
        report.newtonIterations = 7;
        return report;
    }
    CellData cellData(const std::vector<std::string>& fields,
                      bool siUnits,
                      const CellSelection& selection) const
    {
        CellData data;
        for (const auto& name : fields) {
            if (name != "PRESSURE") {
                throw BadRequest("Unknown field " + name);
            }
            CellField field;
            field.name = name;
            field.unit = siUnits ? "PA" : "BARSA";
            field.values.resize(cells);
            for (std::size_t i = 0; i < cells; ++i) {
                field.values[i] = static_cast<double>(i);
            }
            data.fields.push_back(std::move(field));
        }
        if (selection.mode == CellSelection::Mode::Box) {
            data.cells = {4, 5, 6};
            for (auto& field : data.fields) {
                field.values = {4.0, 5.0, 6.0};
            }
        }
        return data;
    }
    WellData wellData(const std::vector<std::string>&) const
    {
        WellData data;
        WellValues prod;
        prod.name = "PROD";
        prod.status = "OPEN";
        prod.control = "ORAT";
        prod.oilRate = -100.0;
        data.wells.push_back(prod);
        data.liquidRateUnit = "SM3/DAY";
        return data;
    }
    SummaryData summary(const std::vector<std::string>& keys, bool history) const
    {
        SummaryData data;
        const int first = history ? 0 : current;
        for (int i = first; i <= current; ++i) {
            data.reportSteps.push_back(i);
            data.elapsedSeconds.push_back(86400.0 * i);
        }
        for (const auto& key : keys) {
            SummarySeries series;
            series.key = key;
            for (int i = first; i <= current; ++i) {
                series.values.push_back(10.0 * i);
            }
            data.series.push_back(std::move(series));
        }
        return data;
    }

    bool hasSummaryKey(const std::string& key) const
    {
        return key == "FOPR";
    }
    std::optional<double> summaryValue(const std::string& key) const
    {
        if (key != "FOPR") {
            return std::nullopt;
        }
        return 100.0 - 10.0 * current;
    }

    ScheduleChange applySchedule(const std::string& text)
    {
        if (text.find("NOSUCHWELL") != std::string::npos) {
            throw BadRequest("No well NOSUCHWELL");
        }
        appliedText = text;
        ScheduleChange change;
        change.text = text;
        change.keywords = {text.substr(0, text.find('\n'))};
        change.reportStep = current;
        change.wells = {"PROD"};
        return change;
    }

    std::string appliedText;
    std::size_t cells = 600000;
    std::chrono::milliseconds stepTime {0};
};

Options
loopback(std::chrono::seconds idle = std::chrono::seconds(30))
{
    Options options;
    options.endpoint = "127.0.0.1:0";
    options.idleTimeout = idle;
    return options;
}

std::unique_ptr<pb::Control::Stub>
connect(const ControlServer& server)
{
    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()),
                                       grpc::InsecureChannelCredentials());
    // A first call on a channel still connecting can fail at once on a busy
    // machine.
    BOOST_REQUIRE(
        channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(30)));
    return pb::Control::NewStub(channel);
}

// Server and loop running on a separate thread, as rank 0 would.
struct Fixture {
    explicit Fixture(const Options& options = loopback())
        : server(startControlServer(options, SerialComm {}))
        , client(connect(*server))
        , exitStatus(std::async(std::launch::async, [this] {
            return runControlLoop(driver, SerialComm {}, server.get());
        }))
    {
    }

    FakeDriver driver;
    std::unique_ptr<ControlServer> server;
    std::unique_ptr<pb::Control::Stub> client;
    std::future<int> exitStatus;
};

pb::StatusReply
step(pb::Control::Stub& client, grpc::Status& status)
{
    grpc::ClientContext context;
    pb::StatusReply reply;
    status = client.Step(&context, pb::StepRequest {}, &reply);
    return reply;
}

grpc::Status
shutdown(pb::Control::Stub& client)
{
    grpc::ClientContext context;
    pb::ShutdownReply reply;
    return client.Shutdown(&context, pb::ShutdownRequest {}, &reply);
}

} // namespace

BOOST_AUTO_TEST_CASE(InfoRoundTrip)
{
    Fixture f;
    grpc::ClientContext context;
    pb::InfoReply info;
    BOOST_REQUIRE(f.client->GetInfo(&context, pb::GetInfoRequest {}, &info).ok());
    BOOST_CHECK_EQUAL(info.engine_version(), "test");
    BOOST_CHECK_EQUAL(info.grid_dims_size(), 3);
    BOOST_CHECK_EQUAL(info.grid_dims(2), 3);
    BOOST_CHECK_EQUAL(info.active_cells(), 300);
    BOOST_CHECK_EQUAL(info.report_dates_size(), 2);
    BOOST_CHECK_EQUAL(info.unit_system(), "METRIC");

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
}

BOOST_AUTO_TEST_CASE(StepToEnd)
{
    Fixture f;
    grpc::Status status;
    for (int i = 1; i <= 3; ++i) {
        const auto reply = step(*f.client, status);
        BOOST_REQUIRE(status.ok());
        BOOST_CHECK_EQUAL(reply.report_step(), i);
        BOOST_CHECK_EQUAL(reply.report_steps_total(), 3);
        BOOST_CHECK_EQUAL(reply.state(), i < 3 ? pb::STATE_STEPPING : pb::STATE_DONE);
    }

    step(*f.client, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

    grpc::ClientContext getStatus;
    pb::StatusReply current;
    BOOST_REQUIRE(f.client->GetStatus(&getStatus, pb::GetStatusRequest {}, &current).ok());
    BOOST_CHECK_EQUAL(current.state(), pb::STATE_DONE);
    BOOST_CHECK_CLOSE(current.elapsed_seconds(), 3 * 86400.0, 1e-12);

    grpc::ClientContext finish;
    pb::StatusReply finished;
    BOOST_REQUIRE(f.client->Finish(&finish, pb::FinishRequest {}, &finished).ok());
    BOOST_CHECK_EQUAL(finished.state(), pb::STATE_FINISHED);

    grpc::ClientContext again;
    BOOST_CHECK_EQUAL(f.client->Finish(&again, pb::FinishRequest {}, &finished).error_code(),
                      grpc::StatusCode::FAILED_PRECONDITION);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
    BOOST_CHECK(f.driver.finished);
}

BOOST_AUTO_TEST_CASE(ShutdownFinishesTheRun)
{
    Fixture f;
    grpc::Status status;
    step(*f.client, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
    BOOST_CHECK(f.driver.finished);
}

BOOST_AUTO_TEST_CASE(SimulatorFailure)
{
    Fixture f;
    f.driver.failAt = 1;
    grpc::Status status;
    step(*f.client, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    step(*f.client, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INTERNAL);
    BOOST_CHECK_EQUAL(status.error_message(), "Linear solver failed");

    grpc::ClientContext getStatus;
    pb::StatusReply current;
    BOOST_REQUIRE(f.client->GetStatus(&getStatus, pb::GetStatusRequest {}, &current).ok());
    BOOST_CHECK_EQUAL(current.state(), pb::STATE_FAILED);
    BOOST_CHECK_EQUAL(current.report_step(), 1);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_FAILURE);
    BOOST_CHECK(!f.driver.finished);
}

BOOST_AUTO_TEST_CASE(IdleTimeoutFinishes)
{
    Fixture f(loopback(std::chrono::seconds(1)));
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
    BOOST_CHECK(f.driver.finished);
}

BOOST_AUTO_TEST_CASE(SecondCommandIsBusy)
{
    auto server = std::make_unique<ControlServer>(loopback());
    auto first = connect(*server);
    auto second = connect(*server);

    grpc::Status firstStatus;
    auto pending = std::async(std::launch::async, [&] { return step(*first, firstStatus); });

    const auto command = server->waitForCommand();
    BOOST_REQUIRE(command.has_value());
    BOOST_CHECK(command->kind == CommandKind::Step);

    grpc::Status secondStatus;
    step(*second, secondStatus);
    BOOST_CHECK_EQUAL(secondStatus.error_code(), grpc::StatusCode::UNAVAILABLE);

    Reply reply;
    reply.status.reportStep = 1;
    server->reply(reply);
    BOOST_CHECK_EQUAL(pending.get().report_step(), 1);
    BOOST_CHECK(firstStatus.ok());
}

BOOST_AUTO_TEST_CASE(TokenIsChecked)
{
    Options options = loopback();
    options.token = "secret";
    ControlServer server(options);
    auto client = connect(server);

    grpc::ClientContext none;
    pb::StatusReply reply;
    BOOST_CHECK_EQUAL(client->GetStatus(&none, pb::GetStatusRequest {}, &reply).error_code(),
                      grpc::StatusCode::UNAUTHENTICATED);

    grpc::ClientContext wrong;
    wrong.AddMetadata("authorization", "Bearer guess");
    BOOST_CHECK_EQUAL(client->GetStatus(&wrong, pb::GetStatusRequest {}, &reply).error_code(),
                      grpc::StatusCode::UNAUTHENTICATED);

    grpc::ClientContext right;
    right.AddMetadata("authorization", "Bearer secret");
    BOOST_CHECK(client->GetStatus(&right, pb::GetStatusRequest {}, &reply).ok());
}

BOOST_AUTO_TEST_CASE(RemoteEndpointNeedsToken)
{
    Options options;
    options.endpoint = "0.0.0.0:0";
    BOOST_CHECK_THROW(ControlServer {options}, std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PendingRequestReleasedOnDestruction)
{
    auto server = std::make_unique<ControlServer>(loopback());
    auto client = connect(*server);
    grpc::Status status;
    auto pending = std::async(std::launch::async, [&] { return step(*client, status); });

    BOOST_REQUIRE(server->waitForCommand().has_value());
    server.reset();
    pending.get();
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

namespace
{

std::vector<pb::CellDataChunk>
readCells(pb::Control::Stub& client, const pb::CellDataRequest& request, grpc::Status& status)
{
    grpc::ClientContext context;
    auto reader = client.GetCellData(&context, request);
    std::vector<pb::CellDataChunk> chunks;
    pb::CellDataChunk chunk;
    while (reader->Read(&chunk)) {
        chunks.push_back(chunk);
    }
    status = reader->Finish();
    return chunks;
}

} // namespace

BOOST_AUTO_TEST_CASE(CellDataIsStreamedInChunks)
{
    Fixture f;
    pb::CellDataRequest request;
    request.add_fields("PRESSURE");
    grpc::Status status;
    const auto chunks = readCells(*f.client, request, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    BOOST_REQUIRE_EQUAL(chunks.size(), 3U);

    std::vector<double> values;
    for (const auto& chunk : chunks) {
        BOOST_CHECK_EQUAL(chunk.field(), "PRESSURE");
        BOOST_CHECK_EQUAL(chunk.unit(), "BARSA");
        BOOST_CHECK_EQUAL(chunk.total(), 600000);
        BOOST_CHECK_EQUAL(chunk.offset(), static_cast<std::int64_t>(values.size()));
        values.insert(values.end(), chunk.values().begin(), chunk.values().end());
    }
    BOOST_REQUIRE_EQUAL(values.size(), 600000U);
    BOOST_CHECK_EQUAL(values.front(), 0.0);
    BOOST_CHECK_EQUAL(values.back(), 599999.0);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
}

BOOST_AUTO_TEST_CASE(BoxSelectionSendsCells)
{
    Fixture f;
    pb::CellDataRequest request;
    request.add_fields("PRESSURE");
    request.set_si_units(true);
    auto& box = *request.mutable_box();
    box.set_i1(1);
    box.set_i2(1);
    box.set_j1(1);
    box.set_j2(1);
    box.set_k1(1);
    box.set_k2(3);
    grpc::Status status;
    const auto chunks = readCells(*f.client, request, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    BOOST_REQUIRE_EQUAL(chunks.size(), 1U);
    BOOST_CHECK_EQUAL(chunks[0].unit(), "PA");
    BOOST_CHECK_EQUAL(chunks[0].cells_size(), 3);
    BOOST_CHECK_EQUAL(chunks[0].cells(2), 6);
    BOOST_CHECK_EQUAL(chunks[0].values(0), 4.0);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    f.exitStatus.get();
}

BOOST_AUTO_TEST_CASE(BadRequestLeavesTheRunUsable)
{
    Fixture f;
    pb::CellDataRequest request;
    request.add_fields("NOSUCHFIELD");
    grpc::Status status;
    readCells(*f.client, request, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    BOOST_CHECK_EQUAL(status.error_message(), "Unknown field NOSUCHFIELD");

    pb::CellDataRequest empty;
    readCells(*f.client, empty, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    const auto reply = step(*f.client, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    BOOST_CHECK_EQUAL(reply.state(), pb::STATE_STEPPING);
    BOOST_CHECK_EQUAL(reply.last_step().time_steps(), 2);
    BOOST_CHECK_EQUAL(reply.last_step().cuts(), 1);
    BOOST_CHECK_EQUAL(reply.last_step().newton_iterations(), 7);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
}

BOOST_AUTO_TEST_CASE(WellsAndSummary)
{
    Fixture f;
    grpc::Status status;
    step(*f.client, status);
    step(*f.client, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());

    grpc::ClientContext wellContext;
    pb::WellDataReply wells;
    BOOST_REQUIRE(f.client->GetWellData(&wellContext, pb::WellDataRequest {}, &wells).ok());
    BOOST_REQUIRE_EQUAL(wells.wells_size(), 1);
    BOOST_CHECK_EQUAL(wells.wells(0).name(), "PROD");
    BOOST_CHECK_EQUAL(wells.wells(0).control(), "ORAT");
    BOOST_CHECK_EQUAL(wells.wells(0).oil_rate(), -100.0);
    BOOST_CHECK_EQUAL(wells.liquid_rate_unit(), "SM3/DAY");

    pb::SummaryRequest request;
    request.add_keys("FOPR");
    request.set_history(true);
    grpc::ClientContext historyContext;
    pb::SummaryReply history;
    BOOST_REQUIRE(f.client->GetSummary(&historyContext, request, &history).ok());
    BOOST_CHECK_EQUAL(history.report_steps_size(), 3);
    BOOST_REQUIRE_EQUAL(history.series_size(), 1);
    BOOST_CHECK_EQUAL(history.series(0).values(2), 20.0);

    request.set_history(false);
    grpc::ClientContext latestContext;
    pb::SummaryReply latest;
    BOOST_REQUIRE(f.client->GetSummary(&latestContext, request, &latest).ok());
    BOOST_CHECK_EQUAL(latest.report_steps_size(), 1);
    BOOST_CHECK_EQUAL(latest.report_steps(0), 2);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    f.exitStatus.get();
}

BOOST_AUTO_TEST_CASE(NoReadsAfterFailure)
{
    Fixture f;
    f.driver.failAt = 0;
    grpc::Status status;
    step(*f.client, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INTERNAL);

    grpc::ClientContext context;
    pb::WellDataReply wells;
    BOOST_CHECK_EQUAL(f.client->GetWellData(&context, pb::WellDataRequest {}, &wells).error_code(),
                      grpc::StatusCode::FAILED_PRECONDITION);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_FAILURE);
}

namespace
{

std::vector<pb::StatusReply>
advance(pb::Control::Stub& client, const pb::AdvanceRequest& request, grpc::Status& status)
{
    grpc::ClientContext context;
    auto reader = client.AdvanceTo(&context, request);
    std::vector<pb::StatusReply> messages;
    pb::StatusReply message;
    while (reader->Read(&message)) {
        messages.push_back(message);
    }
    status = reader->Finish();
    return messages;
}

} // namespace

BOOST_AUTO_TEST_CASE(AdvanceToReportStep)
{
    Fixture f;
    f.driver.steps = 5;
    pb::AdvanceRequest request;
    request.set_report_step(3);
    grpc::Status status;
    const auto messages = advance(*f.client, request, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    // One message per report step. The last says why it stopped.
    BOOST_REQUIRE_EQUAL(messages.size(), 3U);
    BOOST_CHECK_EQUAL(messages[0].report_step(), 1);
    BOOST_CHECK(messages[0].stop_reason().empty());
    BOOST_CHECK_EQUAL(messages[2].report_step(), 3);
    BOOST_CHECK_EQUAL(messages[2].stop_reason(), "Reached report step 3");
    BOOST_CHECK_EQUAL(messages[2].state(), pb::STATE_STEPPING);

    // A target already reached is refused and the run carries on.
    advance(*f.client, request, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    request.set_report_step(9);
    advance(*f.client, request, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    BOOST_CHECK_EQUAL(status.error_message(), "The schedule has 5 report steps");

    request.set_report_step(5);
    const auto rest = advance(*f.client, request, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    BOOST_CHECK_EQUAL(rest.back().state(), pb::STATE_DONE);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
}

BOOST_AUTO_TEST_CASE(AdvanceToDate)
{
    Fixture f;
    pb::AdvanceRequest request;
    // The fake schedule has dates for report steps 0 and 1.
    request.set_date("2026-01-15T00:00:00Z");
    grpc::Status status;
    const auto messages = advance(*f.client, request, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    BOOST_CHECK_EQUAL(messages.back().report_step(), 1);

    request.set_date("2030-01-01T00:00:00Z");
    advance(*f.client, request, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    request.set_date("1 January 2027");
    advance(*f.client, request, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    f.exitStatus.get();
}

BOOST_AUTO_TEST_CASE(AdvanceStopsOnCondition)
{
    Fixture f;
    f.driver.steps = 10;
    pb::AdvanceRequest request;
    request.set_report_step(10);
    auto& stop = *request.add_stop();
    stop.set_key("FOPR");
    stop.set_comparison(pb::StopCondition::COMPARISON_BELOW);
    stop.set_value(75.0);
    grpc::Status status;
    const auto messages = advance(*f.client, request, status);
    BOOST_REQUIRE_MESSAGE(status.ok(), status.error_message());
    // FOPR is 100 - 10 * step, so it first drops below 75 after step 3.
    BOOST_CHECK_EQUAL(messages.back().report_step(), 3);
    BOOST_CHECK_EQUAL(messages.back().stop_reason(), "FOPR below 75.000000");

    stop.set_key("NOSUCHKEY");
    advance(*f.client, request, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    stop.set_key("FOPR");
    stop.set_comparison(pb::StopCondition::COMPARISON_UNSPECIFIED);
    advance(*f.client, request, status);
    BOOST_CHECK_EQUAL(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    f.exitStatus.get();
}

BOOST_AUTO_TEST_CASE(CancelStopsAnAdvance)
{
    Fixture f;
    f.driver.steps = 1000;
    f.driver.stepTime = std::chrono::milliseconds(5);
    pb::AdvanceRequest request;
    request.set_report_step(1000);
    grpc::ClientContext context;
    auto reader = f.client->AdvanceTo(&context, request);
    pb::StatusReply message;
    BOOST_REQUIRE(reader->Read(&message));

    grpc::ClientContext cancel;
    pb::CancelReply cancelled;
    BOOST_REQUIRE(f.client->Cancel(&cancel, pb::CancelRequest {}, &cancelled).ok());

    pb::StatusReply last;
    while (reader->Read(&message)) {
        last = message;
    }
    BOOST_REQUIRE(reader->Finish().ok());
    BOOST_CHECK_EQUAL(last.stop_reason(), "Cancelled");
    BOOST_CHECK_LT(last.report_step(), 1000);

    // The run is still usable.
    grpc::Status status;
    step(*f.client, status);
    BOOST_CHECK(status.ok());

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
}

BOOST_AUTO_TEST_CASE(AbortWhenIdle)
{
    Fixture f;
    grpc::ClientContext context;
    pb::AbortRequest request;
    request.set_reason("Client restarted");
    pb::AbortReply reply;
    BOOST_REQUIRE(f.client->Abort(&context, request, &reply).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_FAILURE);
    BOOST_CHECK(!f.driver.finished);
}

BOOST_AUTO_TEST_CASE(AbortDuringAdvance)
{
    Fixture f;
    f.driver.steps = 1000;
    f.driver.stepTime = std::chrono::milliseconds(5);
    pb::AdvanceRequest request;
    request.set_report_step(1000);
    grpc::ClientContext context;
    auto reader = f.client->AdvanceTo(&context, request);
    pb::StatusReply message;
    BOOST_REQUIRE(reader->Read(&message));

    grpc::ClientContext abort;
    pb::AbortReply aborted;
    BOOST_REQUIRE(f.client->Abort(&abort, pb::AbortRequest {}, &aborted).ok());

    pb::StatusReply last;
    while (reader->Read(&message)) {
        last = message;
    }
    reader->Finish();
    BOOST_CHECK_EQUAL(last.stop_reason(), "Aborted");
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_FAILURE);
    BOOST_CHECK(!f.driver.finished);
}

namespace
{

grpc::Status
setWellControl(pb::Control::Stub& client,
               const pb::WellControlRequest& request,
               pb::ApplyScheduleReply& reply)
{
    grpc::ClientContext context;
    return client.SetWellControl(&context, request, &reply);
}

} // namespace

BOOST_AUTO_TEST_CASE(ApplyScheduleAtNextStep)
{
    Fixture f;
    grpc::Status status;
    step(*f.client, status);
    step(*f.client, status);

    pb::ApplyScheduleRequest request;
    request.set_text("WELOPEN\n 'PROD' 'SHUT' /\n/\n");
    pb::ApplyScheduleReply reply;
    grpc::ClientContext context;
    BOOST_REQUIRE(f.client->ApplySchedule(&context, request, &reply).ok());
    BOOST_CHECK_EQUAL(reply.report_step(), 2);
    BOOST_CHECK_EQUAL(reply.keywords(0), "WELOPEN");
    BOOST_CHECK_EQUAL(reply.wells(0), "PROD");

    request.set_text("WELOPEN\n 'NOSUCHWELL' 'SHUT' /\n/\n");
    grpc::ClientContext refused;
    BOOST_CHECK_EQUAL(f.client->ApplySchedule(&refused, request, &reply).error_code(),
                      grpc::StatusCode::INVALID_ARGUMENT);

    request.set_text("  \n");
    grpc::ClientContext empty;
    BOOST_CHECK_EQUAL(f.client->ApplySchedule(&empty, request, &reply).error_code(),
                      grpc::StatusCode::INVALID_ARGUMENT);

    // No change once the schedule has ended.
    step(*f.client, status);
    BOOST_REQUIRE_EQUAL(f.driver.current, 3);
    request.set_text("WELOPEN\n 'PROD' 'OPEN' /\n/\n");
    grpc::ClientContext late;
    BOOST_CHECK_EQUAL(f.client->ApplySchedule(&late, request, &reply).error_code(),
                      grpc::StatusCode::FAILED_PRECONDITION);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    BOOST_CHECK_EQUAL(f.exitStatus.get(), EXIT_SUCCESS);
}

BOOST_AUTO_TEST_CASE(SetWellControlWritesRecords)
{
    Fixture f;
    pb::ApplyScheduleReply reply;

    pb::WellControlRequest producer;
    producer.set_well("PROD");
    producer.set_control("ORAT");
    producer.set_oil_rate(15000);
    producer.set_bhp(1000.5);
    BOOST_REQUIRE(setWellControl(*f.client, producer, reply).ok());
    BOOST_CHECK_EQUAL(reply.text(),
                      "WCONPROD\n 'PROD' 'OPEN' 'ORAT' 15000 1* 1* 1* 1* 1000.5 1* /\n/\n");
    BOOST_CHECK_EQUAL(f.driver.appliedText, reply.text());

    pb::WellControlRequest injector;
    injector.set_well("INJ");
    injector.set_injector_type("GAS");
    injector.set_status("SHUT");
    injector.set_control("RATE");
    injector.set_surface_rate(80000);
    injector.set_bhp(9014);
    BOOST_REQUIRE(setWellControl(*f.client, injector, reply).ok());
    BOOST_CHECK_EQUAL(reply.text(), "WCONINJE\n 'INJ' 'GAS' 'SHUT' 'RATE' 80000 1* 9014 1* /\n/\n");

    // The control mode needs its target.
    producer.clear_oil_rate();
    BOOST_CHECK_EQUAL(setWellControl(*f.client, producer, reply).error_code(),
                      grpc::StatusCode::INVALID_ARGUMENT);

    producer.set_oil_rate(15000);
    producer.set_control("RATE");
    BOOST_CHECK_EQUAL(setWellControl(*f.client, producer, reply).error_code(),
                      grpc::StatusCode::INVALID_ARGUMENT);

    producer.set_control("ORAT");
    producer.set_well("P'1");
    BOOST_CHECK_EQUAL(setWellControl(*f.client, producer, reply).error_code(),
                      grpc::StatusCode::INVALID_ARGUMENT);

    producer.set_well("PROD");
    producer.set_oil_rate(std::numeric_limits<double>::infinity());
    BOOST_CHECK_EQUAL(setWellControl(*f.client, producer, reply).error_code(),
                      grpc::StatusCode::INVALID_ARGUMENT);

    injector.set_injector_type("STEAM");
    BOOST_CHECK_EQUAL(setWellControl(*f.client, injector, reply).error_code(),
                      grpc::StatusCode::INVALID_ARGUMENT);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    f.exitStatus.get();
}

BOOST_AUTO_TEST_CASE(SetInjectionCompositionWritesRecords)
{
    Fixture f;
    pb::ApplyScheduleReply reply;

    pb::InjectionCompositionRequest request;
    request.set_well("INJ");
    request.set_salt_concentration(15.5);
    (*request.mutable_tracers())["SEA"] = 1.0;
    (*request.mutable_tracers())["FW"] = 0.25;
    request.set_temperature(80);
    grpc::ClientContext context;
    BOOST_REQUIRE(f.client->SetInjectionComposition(&context, request, &reply).ok());
    // Tracers in name order, whatever order the map holds them in.
    BOOST_CHECK_EQUAL(reply.text(),
                      "WSALT\n 'INJ' 15.5 /\n/\nWTRACER\n 'INJ' 'FW' 0.25 /\n 'INJ' 'SEA' 1 /\n/\n"
                      "WTEMP\n 'INJ' 80 /\n/\n");
    BOOST_CHECK_EQUAL(f.driver.appliedText, reply.text());

    const auto refused = [&f](const pb::InjectionCompositionRequest& r) {
        grpc::ClientContext c;
        pb::ApplyScheduleReply out;
        return f.client->SetInjectionComposition(&c, r, &out).error_code();
    };

    pb::InjectionCompositionRequest nothing;
    nothing.set_well("INJ");
    BOOST_CHECK_EQUAL(refused(nothing), grpc::StatusCode::INVALID_ARGUMENT);

    pb::InjectionCompositionRequest negative = request;
    negative.set_salt_concentration(-1.0);
    BOOST_CHECK_EQUAL(refused(negative), grpc::StatusCode::INVALID_ARGUMENT);

    pb::InjectionCompositionRequest hot;
    hot.set_well("INJ");
    hot.set_temperature(std::numeric_limits<double>::quiet_NaN());
    BOOST_CHECK_EQUAL(refused(hot), grpc::StatusCode::INVALID_ARGUMENT);
    hot.set_temperature(120);
    grpc::ClientContext temperatureOnly;
    BOOST_REQUIRE(f.client->SetInjectionComposition(&temperatureOnly, hot, &reply).ok());
    BOOST_CHECK_EQUAL(reply.text(), "WTEMP\n 'INJ' 120 /\n/\n");

    pb::InjectionCompositionRequest badTracer = request;
    (*badTracer.mutable_tracers())["SEA WATER"] = 1.0;
    BOOST_CHECK_EQUAL(refused(badTracer), grpc::StatusCode::INVALID_ARGUMENT);

    pb::InjectionCompositionRequest noWell = request;
    noWell.clear_well();
    BOOST_CHECK_EQUAL(refused(noWell), grpc::StatusCode::INVALID_ARGUMENT);

    BOOST_REQUIRE(shutdown(*f.client).ok());
    f.exitStatus.get();
}
