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

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

// Steps a flow run under control to the end of its schedule, then finishes
// and shuts it down. Used by the control tests.

namespace
{

namespace pb = opm::control::v1;

std::string
readToken(const char* path)
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

} // namespace

int
main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: control_step_client ENDPOINT [TOKEN_FILE]\n";
        return EXIT_FAILURE;
    }
    const std::string token = argc == 3 ? readToken(argv[2]) : std::string {};

    const auto channel = grpc::CreateChannel(argv[1], grpc::InsecureChannelCredentials());
    if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::minutes(5))) {
        std::cerr << "Cannot connect to " << argv[1] << '\n';
        return EXIT_FAILURE;
    }
    const auto stub = pb::Control::NewStub(channel);

    const auto context = [&token] {
        auto result = std::make_unique<grpc::ClientContext>();
        if (!token.empty()) {
            result->AddMetadata("authorization", "Bearer " + token);
        }
        return result;
    };

    pb::InfoReply info;
    if (!check(stub->GetInfo(context().get(), pb::GetInfoRequest {}, &info), "GetInfo")) {
        return EXIT_FAILURE;
    }
    std::cout << "engine " << info.engine_version() << ", ranks " << info.ranks() << ", threads "
              << info.threads_per_rank() << ", active cells " << info.active_cells() << '\n';

    pb::StatusReply status;
    do {
        if (!check(stub->Step(context().get(), pb::StepRequest {}, &status), "Step")) {
            return EXIT_FAILURE;
        }
        std::cout << "step " << status.report_step() << '/' << status.report_steps_total()
                  << ", wall " << status.last_step_wall_seconds() << " s\n";
    } while (status.state() == pb::STATE_STEPPING);

    if (!check(stub->Finish(context().get(), pb::FinishRequest {}, &status), "Finish")) {
        return EXIT_FAILURE;
    }
    pb::ShutdownReply done;
    return check(stub->Shutdown(context().get(), pb::ShutdownRequest {}, &done), "Shutdown")
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
}
