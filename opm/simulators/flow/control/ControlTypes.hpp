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
#include <stdexcept>
#include <string>
#include <vector>

namespace Opm::Control
{

enum class CommandKind : int {
    GetInfo,
    Step,
    Finish,
    Shutdown,
    GetCellData,
    GetWellData,
    GetSummary,
    AdvanceTo,
    Abort,
    ApplySchedule,
    // Issued by the loop itself when the client is gone and the run should
    // carry on to the end.
    FinishSchedule,
};

// Cells to return from GetCellData. All active cells when empty.
struct CellSelection {
    enum class Mode : int { All, Indices, Box };
    Mode mode = Mode::All;
    std::vector<std::int64_t> indices;
    // 1-based inclusive i1, i2, j1, j2, k1, k2.
    std::array<int, 6> box {};
};

struct StopCondition {
    std::string key;
    bool above = false;
    double value = 0.0;
};

// Only kind, fields and siUnits reach every rank. The rest is used on rank
// 0, which sends the other ranks what they need.
struct Command {
    CommandKind kind = CommandKind::Shutdown;
    std::vector<std::string> fields;
    bool siUnits = false;
    CellSelection cells;
    std::vector<std::string> names;
    bool history = false;
    // AdvanceTo: a report step, or a date when targetStep is negative.
    int targetStep = -1;
    std::string date;
    std::vector<StopCondition> stops;
    std::string reason;
    // ApplySchedule: SCHEDULE text, sent to every rank.
    std::string text;
};

// A request the simulator cannot answer, such as an unknown field. Thrown
// before any communication and leaves the simulation untouched.
class BadRequest : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
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

struct StepReport {
    int timeSteps = 0;
    int cuts = 0;
    int newtonIterations = 0;
    int linearIterations = 0;
    int wastedNewtonIterations = 0;
};

struct Status {
    State state = State::Ready;
    int reportStep = 0;
    int reportStepsTotal = 0;
    double elapsedSeconds = 0.0;
    double lastStepWallSeconds = 0.0;
    StepReport lastStep;
    std::string error;
    std::string stopReason;
};

struct CellField {
    std::string name;
    std::string unit;
    std::vector<double> values;
};

struct CellData {
    std::vector<CellField> fields;
    // Active indices of the selected cells, set for a box selection.
    std::vector<std::int64_t> cells;
};

struct WellValues {
    std::string name;
    std::string status;
    bool injector = false;
    std::string control;
    double oilRate = 0.0;
    double waterRate = 0.0;
    double gasRate = 0.0;
    double bhp = 0.0;
    double thp = 0.0;
    double temperature = 0.0;
    double saltRate = 0.0;
};

struct WellData {
    std::vector<WellValues> wells;
    std::string liquidRateUnit;
    std::string gasRateUnit;
    std::string pressureUnit;
    std::string temperatureUnit;
    std::string saltRateUnit;
};

struct SummarySeries {
    std::string key;
    std::vector<double> values;
};

struct SummaryData {
    std::vector<int> reportSteps;
    std::vector<double> elapsedSeconds;
    std::vector<SummarySeries> series;
};

struct ScheduleChange {
    std::string text;
    std::vector<std::string> keywords;
    int reportStep = 0;
    std::vector<std::string> wells;
};

struct Reply {
    ErrorCode code = ErrorCode::None;
    std::string message;
    Info info;
    Status status;
    CellData cellData;
    WellData wellData;
    SummaryData summary;
    ScheduleChange schedule;
};

// What to do when no client has called for the idle timeout.
enum class OnDisconnect : int {
    // Write the output so far and exit.
    Pause,
    // Run the rest of the schedule, then exit.
    Finish,
    // Exit with a non-zero code, without the final output.
    Abort,
};

struct Options {
    std::string endpoint;
    std::string token;
    std::chrono::seconds idleTimeout {900};
    OnDisconnect onDisconnect = OnDisconnect::Pause;
    // PEM contents. TLS is used when both are set.
    std::string tlsCertificate;
    std::string tlsKey;
};

} // namespace Opm::Control

#endif // OPM_CONTROL_TYPES_HPP
