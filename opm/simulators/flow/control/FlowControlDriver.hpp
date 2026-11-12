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

#ifndef OPM_FLOW_CONTROL_DRIVER_HPP
#define OPM_FLOW_CONTROL_DRIVER_HPP

#include <opm/common/utility/TimeService.hpp>

#include <opm/input/eclipse/EclipseState/EclipseState.hpp>
#include <opm/input/eclipse/Schedule/Schedule.hpp>
#include <opm/input/eclipse/Units/UnitSystem.hpp>

#include <opm/simulators/flow/control/ControlTypes.hpp>
#include <opm/simulators/utils/moduleVersion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

#if defined(_OPENMP) && _OPENMP
#include <omp.h>
#endif

namespace Opm::Control
{

// Adapts FlowMain to the driver interface of runControlLoop(), using only
// its public step API. Valid after FlowMain has run the init step.
template <class Main>
class FlowControlDriver
{
public:
    explicit FlowControlDriver(Main& main)
        : main_(main)
    {
    }

    Info info()
    {
        const auto& vanguard = main_.getSimulatorPtr()->vanguard();
        const auto& schedule = vanguard.schedule();

        Info info;
        info.engineVersion = moduleVersion();
        info.ranks = vanguard.grid().comm().size();
#if defined(_OPENMP) && _OPENMP
        info.threadsPerRank = omp_get_max_threads();
#else
        info.threadsPerRank = 1;
#endif
        const auto& dims = vanguard.cartesianDimensions();
        for (std::size_t i = 0; i < info.gridDims.size() && i < dims.size(); ++i) {
            info.gridDims[i] = dims[i];
        }
        info.activeCells = static_cast<std::int64_t>(vanguard.globalNumCells());
        for (std::size_t step = 0; step < schedule.size(); ++step) {
            info.reportDates.push_back(isoDate(schedule.simTime(step)));
        }
        info.unitSystem = vanguard.eclState().getDeckUnitSystem().getName();
        return info;
    }

    int reportStep() const
    {
        return main_.getSimTimer()->currentStepNum();
    }

    int reportStepsTotal() const
    {
        return main_.getSimTimer()->numSteps();
    }

    double elapsedSeconds() const
    {
        return main_.getSimTimer()->simulationTimeElapsed();
    }

    bool done() const
    {
        return stopped_ || main_.getSimTimer()->done();
    }

    void step()
    {
        // False when the schedule asks the run to stop, for example EXIT.
        if (main_.executeStep() == 0) {
            stopped_ = true;
        }
    }

    int finish()
    {
        return main_.executeStepsCleanup();
    }

private:
    static std::string isoDate(const time_point& when)
    {
        const std::time_t t = TimeService::to_time_t(when);
        std::tm utc {};
        gmtime_r(&t, &utc);
        std::array<char, 32> text {};
        const std::size_t n = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc);
        return {text.data(), n};
    }

    Main& main_;
    bool stopped_ = false;
};

} // namespace Opm::Control

#endif // OPM_FLOW_CONTROL_DRIVER_HPP
