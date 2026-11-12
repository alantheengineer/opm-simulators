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
#include <cstdlib>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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

    // Runs one command on this rank. Throws if the simulator fails.
    template <class Driver>
    Reply
    execute(CommandKind command, Driver& driver, const Info& info, Status& status, int& exitStatus)
    {
        Reply reply;
        switch (command) {
        case CommandKind::GetInfo:
            reply.info = info;
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
    CommandKind receive(ControlServer* server, const Comm& comm, bool& timedOut)
    {
        int kind = static_cast<int>(CommandKind::Shutdown);
        timedOut = false;
        if (server) {
            const auto command = server->waitForCommand();
            timedOut = !command.has_value();
            kind = static_cast<int>(command.value_or(Command {}).kind);
        }
        comm.broadcast(&kind, 1, 0);
        return static_cast<CommandKind>(kind);
    }

    // Runs the command on this rank. If any rank throws, every rank marks the
    // run as failed.
    template <class Driver, class Comm>
    Reply executeOnAllRanks(CommandKind command,
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
//   int finish();              final output, returns the exit status
//
// Comm must provide rank(), broadcast(int*, int, int) and max(int).
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
        const CommandKind command = detail::receive(server, comm, timedOut);
        Reply reply = detail::executeOnAllRanks(command, driver, comm, info, status, exitStatus);
        if (server) {
            server->publishStatus(status);
            if (!timedOut) {
                server->reply(std::move(reply));
            }
        }
        if (command == CommandKind::Shutdown) {
            break;
        }
    }
    return status.state == State::Failed ? EXIT_FAILURE : exitStatus;
}

} // namespace Opm::Control

#endif // OPM_CONTROL_LOOP_HPP
