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

#ifndef OPM_CONTROL_LOOP_HPP
#define OPM_CONTROL_LOOP_HPP

#include <opm/common/OpmLog/OpmLog.hpp>

#include <opm/simulators/flow/control/ControlServer.hpp>
#include <opm/simulators/flow/control/ControlTypes.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Opm::Control
{

namespace detail
{

    template <class Driver>
    void updateProgress(const Driver& driver, Status& status)
    {
        status.reportStep = driver.reportStep();
        status.reportStepsTotal = driver.reportStepsTotal();
        status.elapsedSeconds = driver.elapsedSeconds();
    }

    inline bool canStep(State state)
    {
        return state == State::Ready || state == State::Stepping;
    }

    inline bool canFinish(State state)
    {
        return canStep(state) || state == State::Done;
    }

    inline bool canRead(State state)
    {
        return state != State::Failed;
    }

    // Sends rank 0's strings to every rank.
    template <class Comm>
    void broadcastStrings(const Comm& comm, std::vector<std::string>& strings)
    {
        int count = static_cast<int>(strings.size());
        comm.broadcast(&count, 1, 0);
        strings.resize(static_cast<std::size_t>(count));
        for (auto& text : strings) {
            int length = static_cast<int>(text.size());
            comm.broadcast(&length, 1, 0);
            text.resize(static_cast<std::size_t>(length));
            comm.broadcast(text.data(), length, 0);
        }
    }

    template <class Driver>
    Reply read(const Command& command, Driver& driver)
    {
        Reply reply;
        switch (command.kind) {
        case CommandKind::GetCellData:
            reply.cellData = driver.cellData(command.fields, command.siUnits, command.cells);
            break;
        case CommandKind::GetWellData:
            reply.wellData = driver.wellData(command.names);
            break;
        case CommandKind::GetSummary:
            reply.summary = driver.summary(command.names, command.history);
            break;
        default:
            break;
        }
        return reply;
    }

    // Runs one command on this rank. Throws BadRequest for a bad request and
    // anything else if the simulator fails.
    template <class Driver>
    Reply execute(
        const Command& command, Driver& driver, const Info& info, Status& status, int& exitStatus)
    {
        Reply reply;
        switch (command.kind) {
        case CommandKind::GetInfo:
            reply.info = info;
            break;
        case CommandKind::GetCellData:
        case CommandKind::GetWellData:
        case CommandKind::GetSummary:
            if (!canRead(status.state)) {
                reply.code = ErrorCode::InvalidState;
                reply.message = "No data after a failure";
                break;
            }
            reply = read(command, driver);
            break;
        case CommandKind::Step: {
            if (!canStep(status.state)) {
                reply.code = ErrorCode::InvalidState;
                reply.message = "No report steps left to run";
                break;
            }
            const auto start = std::chrono::steady_clock::now();
            driver.step();
            const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
            status.lastStepWallSeconds = wall.count();
            status.lastStep = driver.lastStep();
            updateProgress(driver, status);
            status.state = driver.done() ? State::Done : State::Stepping;
            break;
        }
        case CommandKind::Finish:
            if (!canFinish(status.state)) {
                reply.code = ErrorCode::InvalidState;
                reply.message = "Finish is not allowed after the run has finished or failed";
                break;
            }
            exitStatus = driver.finish();
            status.state = State::Finished;
            break;
        case CommandKind::Shutdown:
            if (canFinish(status.state)) {
                exitStatus = driver.finish();
                status.state = State::Finished;
            }
            break;
        }
        return reply;
    }

    // Waits for a command on rank 0 and broadcasts it. Shutdown after the idle
    // timeout.
    template <class Comm>
    Command receive(ControlServer* server, const Comm& comm, bool& timedOut)
    {
        Command command;
        timedOut = false;
        if (server) {
            auto received = server->waitForCommand();
            timedOut = !received.has_value();
            if (received) {
                command = std::move(*received);
            }
        }
        int kind = static_cast<int>(command.kind);
        comm.broadcast(&kind, 1, 0);
        command.kind = static_cast<CommandKind>(kind);
        if (command.kind == CommandKind::GetCellData) {
            broadcastStrings(comm, command.fields);
            int si = command.siUnits ? 1 : 0;
            comm.broadcast(&si, 1, 0);
            command.siUnits = si != 0;
        }
        return command;
    }

    // Runs the command on this rank. If any rank fails, every rank marks the run
    // as failed.
    template <class Driver, class Comm>
    Reply executeOnAllRanks(const Command& command,
                            Driver& driver,
                            const Comm& comm,
                            const Info& info,
                            Status& status,
                            int& exitStatus)
    {
        Reply reply;
        std::string error;
        bool failed = false;
        try {
            reply = execute(command, driver, info, status, exitStatus);
        } catch (const BadRequest& e) {
            // Each rank checks the same request before any communication, so
            // they agree.
            reply = Reply {};
            reply.code = ErrorCode::InvalidArgument;
            reply.message = e.what();
        } catch (const std::exception& e) {
            failed = true;
            error = e.what();
        } catch (...) {
            failed = true;
            error = "Unknown error";
        }

        if (comm.max(failed ? 1 : 0) != 0) {
            status.state = State::Failed;
            status.error = failed ? error : "Failure on another rank";
            reply.code = ErrorCode::Internal;
            reply.message = status.error;
            exitStatus = EXIT_FAILURE;
        }
        reply.status = status;
        return reply;
    }

} // namespace detail

// Starts the server on rank 0 and returns null on the other ranks. Called on
// every rank; throws on every rank if rank 0 cannot start the server.
template <class Comm>
std::unique_ptr<ControlServer>
startControlServer(const Options& options, const Comm& comm)
{
    const bool root = comm.rank() == 0;
    std::unique_ptr<ControlServer> server;
    std::string error;
    if (root) {
        try {
            server = std::make_unique<ControlServer>(options);
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    if (comm.max(root && !server ? 1 : 0) != 0) {
        throw std::runtime_error(root ? error : "Control server failed to start on rank 0");
    }
    if (server) {
        OpmLog::info("Control server listening on port " + std::to_string(server->port()));
    }
    return server;
}

// Runs report steps on request until Shutdown or the idle timeout. Called on
// every rank, with the server on rank 0 and null elsewhere. Rank 0 broadcasts
// each command, so all ranks see the same commands in the same order and keep
// the same state.
//
// Driver must provide:
//   Info info();               collective, called once
//   int reportStep() const;
//   int reportStepsTotal() const;
//   double elapsedSeconds() const;
//   bool done() const;
//   void step();               one report step, collective
//   StepReport lastStep() const;
//   int finish();              final output, returns the exit status
//   CellData cellData(fields, siUnits, selection);   collective, result on rank 0
//   WellData wellData(names);                        result on rank 0
//   SummaryData summary(keys, history);              result on rank 0
//
// Comm must provide rank(), broadcast(T*, int, int) and max(int).
template <class Driver, class Comm>
int
runControlLoop(Driver& driver, const Comm& comm, ControlServer* server)
{
    const Info info = driver.info();

    Status status;
    detail::updateProgress(driver, status);
    status.state = driver.done() ? State::Done : State::Ready;
    if (server) {
        server->publishStatus(status);
    }

    int exitStatus = EXIT_SUCCESS;
    for (;;) {
        bool timedOut = false;
        const Command command = detail::receive(server, comm, timedOut);
        Reply reply = detail::executeOnAllRanks(command, driver, comm, info, status, exitStatus);
        if (server) {
            server->publishStatus(status);
            if (!timedOut) {
                server->reply(std::move(reply));
            }
        }
        if (command.kind == CommandKind::Shutdown) {
            break;
        }
    }
    return status.state == State::Failed ? EXIT_FAILURE : exitStatus;
}

} // namespace Opm::Control

#endif // OPM_CONTROL_LOOP_HPP
