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

#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

namespace pb = opm::control::v1;
using namespace Opm::Control;

struct SerialComm {
    int rank() const
    {
        return 0;
    }
    int broadcast(int*, int, int) const
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
        ++current;
    }
    int finish()
    {
        finished = true;
        return EXIT_SUCCESS;
    }
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
    BOOST_REQUIRE(status.ok());
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
    BOOST_REQUIRE(status.ok());
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
