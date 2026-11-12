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

#include <opm/simulators/flow/control/opm_control.grpc.pb.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Steps a flow run under control to the end of its schedule, reads the wells,
// the field oil rate and the final pressure, then finishes and shuts it down.
// Used by the control tests.
//
// Usage: control_step_client ENDPOINT [--token-file=PATH] [--pressure-file=PATH]

namespace
{

namespace pb = opm::control::v1;

std::string
readToken(const std::string& path)
{
    std::ifstream in(path);
    std::string token {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto end = token.find_last_not_of(" \t\r\n");
    token.erase(end == std::string::npos ? 0 : end + 1);
    return token;
}

bool
check(const grpc::Status& status, const char* what)
{
    if (!status.ok()) {
        std::cerr << what << " failed: " << status.error_message() << '\n';
    }
    return status.ok();
}

class Client
{
public:
    Client(const std::string& endpoint, std::string token)
        : channel_(grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()))
        , stub_(pb::Control::NewStub(channel_))
        , token_(std::move(token))
    {
    }

    bool connect()
    {
        return channel_->WaitForConnected(std::chrono::system_clock::now()
                                          + std::chrono::minutes(5));
    }

    std::unique_ptr<grpc::ClientContext> context() const
    {
        auto result = std::make_unique<grpc::ClientContext>();
        if (!token_.empty()) {
            result->AddMetadata("authorization", "Bearer " + token_);
        }
        return result;
    }

    pb::Control::Stub& stub()
    {
        return *stub_;
    }

private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<pb::Control::Stub> stub_;
    std::string token_;
};

void
printStep(const pb::StatusReply& status)
{
    const auto& step = status.last_step();
    std::cout << "step " << status.report_step() << '/' << status.report_steps_total() << ", "
              << step.time_steps() << " time steps, " << step.cuts() << " cuts, "
              << step.newton_iterations() << " Newton, " << step.linear_iterations()
              << " linear, wall " << status.last_step_wall_seconds() << " s\n";
}

bool
printWells(Client& client)
{
    pb::WellDataReply reply;
    if (!check(client.stub().GetWellData(client.context().get(), pb::WellDataRequest {}, &reply),
               "GetWellData")) {
        return false;
    }
    for (const auto& well : reply.wells()) {
        std::cout << "well " << well.name() << ' ' << well.status() << ' ' << well.control()
                  << ", oil " << well.oil_rate() << ' ' << reply.liquid_rate_unit() << ", gas "
                  << well.gas_rate() << ' ' << reply.gas_rate_unit() << ", bhp " << well.bhp()
                  << ' ' << reply.pressure_unit() << '\n';
    }
    return true;
}

bool
printOilRate(Client& client)
{
    pb::SummaryRequest request;
    request.add_keys("FOPR");
    request.set_history(true);
    pb::SummaryReply reply;
    if (!check(client.stub().GetSummary(client.context().get(), request, &reply), "GetSummary")) {
        return false;
    }
    for (int i = 0; i < reply.report_steps_size(); ++i) {
        std::cout << "FOPR at step " << reply.report_steps(i) << ": " << reply.series(0).values(i)
                  << '\n';
    }
    return true;
}

// Writes the pressure of every active cell, one value per line.
bool
writePressure(Client& client, const std::string& path)
{
    pb::CellDataRequest request;
    request.add_fields("PRESSURE");
    auto context = client.context();
    auto reader = client.stub().GetCellData(context.get(), request);
    std::vector<double> values;
    std::string unit;
    pb::CellDataChunk chunk;
    while (reader->Read(&chunk)) {
        unit = chunk.unit();
        values.insert(values.end(), chunk.values().begin(), chunk.values().end());
    }
    if (!check(reader->Finish(), "GetCellData")) {
        return false;
    }
    std::ofstream out(path);
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const double value : values) {
        out << value << '\n';
    }
    std::cout << "pressure of " << values.size() << " cells in " << unit << " written to " << path
              << '\n';
    return static_cast<bool>(out);
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr
            << "Usage: control_step_client ENDPOINT [--token-file=PATH] [--pressure-file=PATH]\n";
        return EXIT_FAILURE;
    }
    std::string token;
    std::string pressureFile;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg.starts_with("--token-file=")) {
            token = readToken(std::string(arg.substr(13)));
        } else if (arg.starts_with("--pressure-file=")) {
            pressureFile = arg.substr(16);
        } else {
            std::cerr << "Unknown option " << arg << '\n';
            return EXIT_FAILURE;
        }
    }

    Client client(argv[1], token);
    if (!client.connect()) {
        std::cerr << "Cannot connect to " << argv[1] << '\n';
        return EXIT_FAILURE;
    }

    pb::InfoReply info;
    if (!check(client.stub().GetInfo(client.context().get(), pb::GetInfoRequest {}, &info),
               "GetInfo")) {
        return EXIT_FAILURE;
    }
    std::cout << "engine " << info.engine_version() << ", ranks " << info.ranks() << ", threads "
              << info.threads_per_rank() << ", active cells " << info.active_cells() << '\n';

    // The first half with AdvanceTo, the rest one Step at a time, so that
    // both paths are covered.
    pb::StatusReply status;
    pb::AdvanceRequest advance;
    advance.set_report_step(std::max(1, info.report_dates_size() / 2));
    auto advanceContext = client.context();
    auto reader = client.stub().AdvanceTo(advanceContext.get(), advance);
    while (reader->Read(&status)) {
        printStep(status);
    }
    if (!check(reader->Finish(), "AdvanceTo")) {
        return EXIT_FAILURE;
    }
    std::cout << "advance stopped: " << status.stop_reason() << '\n';

    while (status.state() == pb::STATE_STEPPING) {
        if (!check(client.stub().Step(client.context().get(), pb::StepRequest {}, &status),
                   "Step")) {
            return EXIT_FAILURE;
        }
        printStep(status);
    }

    if (!printWells(client) || !printOilRate(client)) {
        return EXIT_FAILURE;
    }
    if (!pressureFile.empty() && !writePressure(client, pressureFile)) {
        return EXIT_FAILURE;
    }

    if (!check(client.stub().Finish(client.context().get(), pb::FinishRequest {}, &status),
               "Finish")) {
        return EXIT_FAILURE;
    }
    pb::ShutdownReply done;
    return check(client.stub().Shutdown(client.context().get(), pb::ShutdownRequest {}, &done),
                 "Shutdown")
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
