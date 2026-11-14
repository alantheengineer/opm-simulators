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
#include <opm/simulators/flow/control/Watchdog.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
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

    template <class Comm>
    bool broadcastFlag(const Comm& comm, bool flag)
    {
        int value = flag ? 1 : 0;
        comm.broadcast(&value, 1, 0);
        return value != 0;
    }

    template <class Driver>
    void runStep(Driver& driver, Status& status)
    {
        const auto start = std::chrono::steady_clock::now();
        driver.step();
        const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
        status.lastStepWallSeconds = wall.count();
        status.lastStep = driver.lastStep();
        updateProgress(driver, status);
        status.state = driver.done() ? State::Done : State::Stepping;
    }

    // Report step to advance to, worked out on rank 0. Throws BadRequest if the
    // request cannot be met.
    template <class Driver>
    int resolveTarget(const Command& command,
                      const Driver& driver,
                      const Info& info,
                      const Status& status)
    {
        int target = command.targetStep;
        if (target < 0) {
            const auto& date = command.date;
            if (date.size() != 20 || date[4] != '-' || date[10] != 'T' || date.back() != 'Z') {
                throw BadRequest("Date " + date + " is not of the form 2027-01-01T00:00:00Z");
            }
            const auto& dates = info.reportDates;
            const auto it = std::find_if(
                dates.begin(), dates.end(), [&date](const auto& d) { return d >= date; });
            if (it == dates.end()) {
                throw BadRequest("Date " + date + " is after the end of the schedule");
            }
            target = static_cast<int>(it - dates.begin());
        }
        if (target <= status.reportStep) {
            throw BadRequest("Report step " + std::to_string(target) + " is already done");
        }
        if (target > status.reportStepsTotal) {
            throw BadRequest("The schedule has " + std::to_string(status.reportStepsTotal)
                             + " report steps");
        }
        for (const auto& stop : command.stops) {
            if (!driver.hasSummaryKey(stop.key)) {
                throw BadRequest("Unknown summary key " + stop.key);
            }
        }
        return target;
    }

    // Why an advance should stop after this report step, decided on rank 0.
    // Empty to carry on.
    template <class Driver>
    std::string
    stopReason(const Command& command, const Driver& driver, const ControlServer& server)
    {
        if (server.abortRequested()) {
            return "Aborted";
        }
        if (server.cancelRequested()) {
            return "Cancelled";
        }
        for (const auto& stop : command.stops) {
            const auto value = driver.summaryValue(stop.key);
            if (value && (stop.above ? *value > stop.value : *value < stop.value)) {
                return stop.key + (stop.above ? " above " : " below ") + std::to_string(stop.value);
            }
        }
        return {};
    }

    template <class Driver, class Comm>
    Reply advance(const Command& command,
                  Driver& driver,
                  const Comm& comm,
                  ControlServer* server,
                  const Info& info,
                  Status& status)
    {
        Reply reply;
        if (!canStep(status.state)) {
            reply.code = ErrorCode::InvalidState;
            reply.message = "No report steps left to run";
            return reply;
        }

        // Rank 0 checks the request and every rank learns the result, so a bad
        // request is refused everywhere.
        int target = -1;
        std::string error;
        if (server) {
            try {
                target = resolveTarget(command, driver, info, status);
            } catch (const BadRequest& e) {
                error = e.what();
            }
        }
        comm.broadcast(&target, 1, 0);
        if (target < 0) {
            throw BadRequest(error.empty() ? "Bad AdvanceTo request" : error);
        }

        // Rank 0 decides after each report step whether to stop and sends the
        // reason with that step's status, so the last one carries it.
        std::string reason;
        for (;;) {
            runStep(driver, status);
            if (server) {
                reason = stopReason(command, driver, *server);
                if (reason.empty() && status.state == State::Done) {
                    reason = "End of schedule";
                } else if (reason.empty() && status.reportStep >= target) {
                    reason = "Reached report step " + std::to_string(status.reportStep);
                }
                status.stopReason = reason;
                server->publishStatus(status);
                server->progress(status);
            }
            if (broadcastFlag(comm, !reason.empty())) {
                break;
            }
        }
        return reply;
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
            reply.wellData = driver.wellData(command.names, command.connections);
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
    template <class Driver, class Comm>
    Reply execute(const Command& command,
                  Driver& driver,
                  const Comm& comm,
                  ControlServer* server,
                  const Info& info,
                  Status& status,
                  int& exitStatus)
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
        case CommandKind::Step:
            if (!canStep(status.state)) {
                reply.code = ErrorCode::InvalidState;
                reply.message = "No report steps left to run";
                break;
            }
            runStep(driver, status);
            break;
        case CommandKind::AdvanceTo:
            reply = advance(command, driver, comm, server, info, status);
            break;
        case CommandKind::ApplySchedule:
            if (!canStep(status.state)) {
                reply.code = ErrorCode::InvalidState;
                reply.message = "No report steps left to change";
                break;
            }
            reply.schedule = driver.applySchedule(command.text);
            break;
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
        case CommandKind::SaveCheckpoint:
            if (!canFinish(status.state)) {
                reply.code = ErrorCode::InvalidState;
                reply.message = "No checkpoint after the run has finished or failed";
                break;
            }
            reply.checkpoint = driver.saveCheckpoint(command.text);
            break;
        case CommandKind::FinishSchedule:
            while (canStep(status.state)) {
                runStep(driver, status);
                if (server) {
                    server->publishStatus(status);
                }
            }
            if (canFinish(status.state)) {
                exitStatus = driver.finish();
                status.state = State::Finished;
            }
            break;
        case CommandKind::Abort:
            status.state = State::Failed;
            status.error = command.reason.empty() ? "Aborted" : "Aborted: " + command.reason;
            exitStatus = EXIT_FAILURE;
            break;
        }
        return reply;
    }

    // Waits for a command on rank 0 and broadcasts it. After the idle timeout
    // the command follows the disconnect policy.
    template <class Comm>
    Command
    receive(ControlServer* server, const Comm& comm, OnDisconnect onDisconnect, bool& timedOut)
    {
        Command command;
        timedOut = false;
        if (server) {
            auto received = server->waitForCommand();
            timedOut = !received.has_value();
            if (received) {
                command = std::move(*received);
            } else if (onDisconnect == OnDisconnect::Finish) {
                command.kind = CommandKind::FinishSchedule;
            } else if (onDisconnect == OnDisconnect::Abort) {
                command.kind = CommandKind::Abort;
                command.reason = "No command within the idle timeout";
            }
        }
        int kind = static_cast<int>(command.kind);
        comm.broadcast(&kind, 1, 0);
        command.kind = static_cast<CommandKind>(kind);
        if (command.kind == CommandKind::GetCellData) {
            broadcastStrings(comm, command.fields);
            command.siUnits = broadcastFlag(comm, command.siUnits);
        }
        if (command.kind == CommandKind::ApplySchedule
            || command.kind == CommandKind::SaveCheckpoint) {
            std::vector<std::string> text {command.text};
            broadcastStrings(comm, text);
            command.text = text.front();
        }
        return command;
    }

    // Runs the command on this rank. If any rank fails, every rank marks the run
    // as failed.
    template <class Driver, class Comm>
    Reply executeOnAllRanks(const Command& command,
                            Driver& driver,
                            const Comm& comm,
                            ControlServer* server,
                            const Info& info,
                            Status& status,
                            int& exitStatus,
                            std::chrono::milliseconds failureTimeout)
    {
        Reply reply;
        std::string error;
        bool failed = false;
        status.stopReason.clear();
        try {
            reply = execute(command, driver, comm, server, info, status, exitStatus);
        } catch (const BadRequest& e) {
            // Every rank either checks the same request or learns rank 0's
            // verdict, so they agree.
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

        // A failure on some ranks only can leave the others waiting in MPI for
        // ranks that have stopped. If the ranks cannot agree in time, end this
        // process so that the MPI launcher ends the rest.
        bool anyFailed = false;
        if (failed) {
            Watchdog watchdog(failureTimeout, [&error] {
                std::cerr << "Step control: ranks did not agree on a failure in time: " << error
                          << std::endl;
                std::_Exit(EXIT_FAILURE);
            });
            anyFailed = comm.max(1) != 0;
        } else {
            anyFailed = comm.max(0) != 0;
        }
        if (anyFailed) {
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
//   WellData wellData(names, connections);           result on rank 0
//   SummaryData summary(keys, history);              result on rank 0
//   bool hasSummaryKey(key) const;                   rank 0
//   ScheduleChange applySchedule(text);              every rank, same result
//   Checkpoint saveCheckpoint(label);                collective
//   std::optional<double> summaryValue(key) const;   rank 0
//
// Comm must provide rank(), broadcast(T*, int, int) and max(int).
template <class Driver, class Comm>
int
runControlLoop(Driver& driver,
               const Comm& comm,
               ControlServer* server,
               OnDisconnect onDisconnect = OnDisconnect::Pause,
               std::chrono::milliseconds failureTimeout = std::chrono::seconds(30))
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
        const Command command = detail::receive(server, comm, onDisconnect, timedOut);
        Reply reply = detail::executeOnAllRanks(
            command, driver, comm, server, info, status, exitStatus, failureTimeout);
        if (server) {
            server->publishStatus(status);
            if (!timedOut) {
                server->reply(std::move(reply));
            }
        }
        if (command.kind == CommandKind::Shutdown || command.kind == CommandKind::Abort
            || command.kind == CommandKind::FinishSchedule) {
            break;
        }
        // Abort during a command ends the run once that command is done.
        if (detail::broadcastFlag(comm, server && server->abortRequested())) {
            status.state = State::Failed;
            exitStatus = EXIT_FAILURE;
            break;
        }
    }
    return status.state == State::Failed ? EXIT_FAILURE : exitStatus;
}

} // namespace Opm::Control

#endif // OPM_CONTROL_LOOP_HPP
