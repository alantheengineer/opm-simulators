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

#ifndef OPM_CONTROL_SERVER_HPP
#define OPM_CONTROL_SERVER_HPP

#include <opm/simulators/flow/control/ControlTypes.hpp>

#include <memory>
#include <optional>
#include <string>

namespace Opm::Control
{

// gRPC endpoint on rank 0. Requests are handed to the simulator thread one at
// a time through waitForCommand() and reply(); GetStatus is answered from the
// last published status without involving the simulator.
class ControlServer
{
public:
    // Throws if the endpoint cannot be bound, or if it is not a loopback
    // address and no token is given.
    explicit ControlServer(const Options& options);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;
    ControlServer(ControlServer&&) = delete;
    ControlServer& operator=(ControlServer&&) = delete;

    // Port actually bound. Differs from the endpoint when it asks for port 0.
    int port() const;

    // Blocks until a command arrives. Empty after the idle timeout.
    std::optional<Command> waitForCommand();

    // Answers the command returned by the last waitForCommand().
    void reply(Reply reply);

    void publishStatus(const Status& status);

    // Sends the status of a report step to the client of a running
    // AdvanceTo.
    void progress(const Status& status);

    // Set by Cancel or Abort since the last command started.
    bool cancelRequested() const;

    // Set by Abort. The reason is empty until then.
    bool abortRequested() const;
    std::string abortReason() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace Opm::Control

#endif // OPM_CONTROL_SERVER_HPP
