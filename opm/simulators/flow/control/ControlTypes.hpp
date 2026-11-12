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

#ifndef OPM_CONTROL_TYPES_HPP
#define OPM_CONTROL_TYPES_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace Opm::Control
{

enum class CommandKind : int { GetInfo, Step, Finish, Shutdown };

// Sent from rank 0 to every rank. Commands carry no arguments yet.
struct Command {
    CommandKind kind = CommandKind::Shutdown;
};

enum class State : int { Ready, Stepping, Done, Finished, Failed };

enum class ErrorCode : int { None, InvalidArgument, InvalidState, Busy, Unauthenticated, Internal };

struct Info {
    std::string engineVersion;
    int ranks = 0;
    int threadsPerRank = 0;
    std::array<int, 3> gridDims {};
    std::int64_t activeCells = 0;
    std::vector<std::string> reportDates;
    std::string unitSystem;
};

struct Status {
    State state = State::Ready;
    int reportStep = 0;
    int reportStepsTotal = 0;
    double elapsedSeconds = 0.0;
    double lastStepWallSeconds = 0.0;
    std::string error;
};

struct Reply {
    ErrorCode code = ErrorCode::None;
    std::string message;
    Info info;
    Status status;
};

struct Options {
    std::string endpoint;
    std::string token;
    std::chrono::seconds idleTimeout {900};
};

} // namespace Opm::Control

#endif // OPM_CONTROL_TYPES_HPP
