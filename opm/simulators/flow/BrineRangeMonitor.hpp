/*
  Copyright 2026 Dr Alan Tominey.

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
#ifndef OPM_BRINE_RANGE_MONITOR_HPP
#define OPM_BRINE_RANGE_MONITOR_HPP

#include <opm/common/utility/ElectrolyteFractions.hpp>

#include <opm/input/eclipse/EclipseState/Runspec.hpp>

#include <opm/material/fluidsystems/blackoilpvt/BrineMulticompPvt.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace Opm
{

/*!
 * \brief Tracks where the SALTMC brine models are evaluated outside their fitted range.
 *
 * Converged cell states are checked after each time step. Once per report step the counts
 * and extremes are combined across ranks and turned into messages, according to the
 * SALTMC range policy: WARN and CLAMP report each region and property once per run, ERROR
 * reports and asks the run to stop.
 */
template <class Scalar>
class BrineRangeMonitor
{
public:
    using Pvt = BrineMulticompPvt<Scalar>;
    using FitProperty = typename Pvt::FitProperty;
    using Policy = Saltmc::RangePolicy;

    struct Report {
        std::vector<std::string> messages;
        bool stop {false};
    };

    void init(const Pvt& pvt, Policy policy, std::size_t numCells)
    {
        pvt_ = &pvt;
        policy_ = policy;
        cellFlags_.assign(numCells, 0);
        excursions_.assign(pvt.numRegions() * numProperties_, Excursion {});
        reported_.assign(pvt.numRegions() * numProperties_, false);
    }

    bool active() const
    {
        return pvt_ != nullptr;
    }

    /// Check one cell's converged state
    void check(std::size_t cell,
               unsigned regionIdx,
               Scalar temperature,
               Scalar pressure,
               Scalar saltConcentration,
               const ElectrolyteFractions* fractions)
    {
        const unsigned flags
            = pvt_->outOfRange(regionIdx, temperature, pressure, saltConcentration, fractions);
        if (flags == 0) {
            return;
        }
        const Scalar ionic = pvt_->ionicStrength(regionIdx, saltConcentration, fractions);
        const Scalar w = pvt_->saltMassFraction(regionIdx, saltConcentration);
        for (std::size_t prop = 0; prop < numProperties_; ++prop) {
            const unsigned bit = propertyBit_(prop);
            if ((flags & bit) == 0) {
                continue;
            }
            auto& e = excursions_[regionIdx * numProperties_ + prop];
            if ((cellFlags_[cell] & bit) == 0) {
                e.cells += 1;
            }
            e.minT = std::min(e.minT, temperature);
            e.maxT = std::max(e.maxT, temperature);
            e.maxP = std::max(e.maxP, pressure);
            e.maxIonic = std::max(e.maxIonic, ionic);
            e.maxW = std::max(e.maxW, w);
        }
        cellFlags_[cell] |= static_cast<unsigned char>(flags);
    }

    /// Combine the report step's excursions across ranks and reset them
    template <class Comm>
    Report endReportStep(const Comm& comm, int reportStep)
    {
        const std::size_t n = excursions_.size();
        std::vector<Scalar> sums(n);
        std::vector<Scalar> maxima(5 * n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto& e = excursions_[i];
            sums[i] = e.cells;
            // The minimum temperature is combined as a maximum of its negative
            maxima[5 * i] = -e.minT;
            maxima[5 * i + 1] = e.maxT;
            maxima[5 * i + 2] = e.maxP;
            maxima[5 * i + 3] = e.maxIonic;
            maxima[5 * i + 4] = e.maxW;
        }
        comm.sum(sums.data(), static_cast<int>(n));
        comm.max(maxima.data(), static_cast<int>(5 * n));

        Report report;
        for (std::size_t i = 0; i < n; ++i) {
            if (sums[i] == 0 || (reported_[i] && policy_ != Policy::Error)) {
                continue;
            }
            const Excursion e {sums[i],
                               -maxima[5 * i],
                               maxima[5 * i + 1],
                               maxima[5 * i + 2],
                               maxima[5 * i + 3],
                               maxima[5 * i + 4]};
            report.messages.push_back(message_(static_cast<unsigned>(i / numProperties_),
                                               static_cast<FitProperty>(i % numProperties_),
                                               e,
                                               reportStep));
            reported_[i] = true;
            report.stop = report.stop || policy_ == Policy::Error;
        }

        std::ranges::fill(cellFlags_, 0);
        std::ranges::fill(excursions_, Excursion {});
        return report;
    }

private:
    static constexpr std::size_t numProperties_ = 2;
    static constexpr Scalar lowest_ = std::numeric_limits<Scalar>::lowest();

    struct Excursion {
        Scalar cells {0};
        Scalar minT {std::numeric_limits<Scalar>::max()};
        Scalar maxT {lowest_};
        Scalar maxP {lowest_};
        Scalar maxIonic {lowest_};
        Scalar maxW {lowest_};
    };

    static unsigned propertyBit_(std::size_t prop)
    {
        return prop == 0 ? Pvt::densityOutOfRange : Pvt::viscosityOutOfRange;
    }

    std::string
    message_(unsigned regionIdx, FitProperty property, const Excursion& e, int step) const
    {
        const auto& r = pvt_->fitRange(regionIdx, property);
        std::vector<std::string> crossed;
        if (e.maxT > r.maxTemperature) {
            crossed.push_back(fmt::format("temperature up to {:.1f} C (fitted to {:.1f} C)",
                                          e.maxT - 273.15,
                                          r.maxTemperature - 273.15));
        }
        if (e.minT < r.minTemperature) {
            crossed.push_back(fmt::format("temperature down to {:.1f} C (fitted from {:.1f} C)",
                                          e.minT - 273.15,
                                          r.minTemperature - 273.15));
        }
        if (e.maxP > r.maxPressure) {
            crossed.push_back(fmt::format("pressure up to {:.1f} MPa (fitted to {:.1f} MPa)",
                                          e.maxP * 1e-6,
                                          r.maxPressure * 1e-6));
        }
        if (e.maxIonic > r.maxIonicStrength) {
            crossed.push_back(
                fmt::format("ionic strength up to {:.2f} mol/kg (fitted to {:.2f} mol/kg)",
                            e.maxIonic,
                            r.maxIonicStrength));
        }
        if (e.maxW > r.maxSaltMassFraction) {
            crossed.push_back(fmt::format("salt mass fraction up to {:.3f} (fitted to {:.3f})",
                                          e.maxW,
                                          r.maxSaltMassFraction));
        }

        std::string text = fmt::format("SALTMC: brine {} in PVT region {} was outside its fitted "
                                       "range in {:.0f} cells in report step {}: {}.",
                                       property == FitProperty::Density ? "density" : "viscosity",
                                       regionIdx + 1,
                                       e.cells,
                                       step,
                                       fmt::join(crossed, ", "));
        switch (policy_) {
        case Policy::Warn:
            text += " The model is extrapolated there. Reported once per region and property.";
            break;
        case Policy::Clamp:
            text += " Temperature and pressure are held at the range edge there; salinity is "
                    "extrapolated. Reported once per region and property.";
            break;
        case Policy::Error:
            text += " Stopping, as SALTMC RANGE_POLICY is ERROR.";
            break;
        }
        return text;
    }

    const Pvt* pvt_ {nullptr};
    Policy policy_ {Policy::Warn};
    std::vector<unsigned char> cellFlags_ {};
    std::vector<Excursion> excursions_ {};
    std::vector<bool> reported_ {};
};

} // namespace Opm

#endif // OPM_BRINE_RANGE_MONITOR_HPP
