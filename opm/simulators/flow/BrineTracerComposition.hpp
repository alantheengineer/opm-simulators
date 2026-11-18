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
#ifndef OPM_BRINE_TRACER_COMPOSITION_HPP
#define OPM_BRINE_TRACER_COMPOSITION_HPP

#include <opm/common/utility/ElectrolyteFractions.hpp>
#include <opm/common/utility/SaltArray.hpp>

#include <opm/material/fluidsystems/blackoilpvt/BrineMulticompPvt.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Opm
{

/*!
 * \brief Per-cell brine electrolyte fractions from ion tracers, for SALTMC TRACER mode.
 *
 * Water tracers named after ions are tracked. Untracked ions keep their SALINITC share of
 * the salt; tracked ions share the rest in proportion to their tracer concentrations. A
 * cell with no tracked tracer keeps the SALINITC ratios. The fractions are rebuilt once
 * per time step and held through the Newton solve.
 *
 * Tracer concentrations are taken as ion mass per surface volume of brine, the unit of the
 * BRINE salt concentration, so their sum can be checked against the total salt.
 */
template <class Scalar>
class BrineTracerComposition
{
public:
    using Pvt = BrineMulticompPvt<Scalar>;
    using Salinity = SaltArray<Scalar, SaltMassFraction>;

    /*!
     * \brief Find the ion tracers.
     *
     * \param names Tracer names
     * \param isWaterTracer Whether each tracer is carried by water
     * \return Whether any tracer is an ion
     */
    bool init(const std::vector<std::string>& names, const std::vector<bool>& isWaterTracer)
    {
        tracers_.clear();
        tracked_.fill(false);
        for (std::size_t tracerIdx = 0; tracerIdx < names.size(); ++tracerIdx) {
            if (!isWaterTracer[tracerIdx]) {
                continue;
            }
            if (const auto ion = saltIndexFromName(names[tracerIdx])) {
                tracers_.push_back({static_cast<int>(tracerIdx), *ion});
                tracked_[static_cast<std::size_t>(*ion)] = true;
            }
        }
        return !tracers_.empty();
    }

    bool active() const
    {
        return !tracers_.empty();
    }

    /*!
     * \brief Rebuild the fractions of every cell.
     *
     * \param tracerConcentration (tracer index, cell) to the tracer concentration
     * \param regionIndex Cell to PVT region
     * \param saltConcentration Cell to total salt [kg/sm3], or nothing when it is not known
     */
    template <class TracerFn, class RegionFn, class SaltFn>
    void update(const Pvt& pvt,
                std::size_t numCells,
                TracerFn&& tracerConcentration,
                RegionFn&& regionIndex,
                SaltFn&& saltConcentration)
    {
        fractions_.resize(numCells);
        maxMismatch_ = 0.0;
        for (std::size_t cell = 0; cell < numCells; ++cell) {
            const unsigned regionIdx = regionIndex(cell);
            const Salinity& region = pvt.salinity(regionIdx);
            const Scalar regionTotal = region.sum();

            // Share of each ion per unit salt
            Salinity ions;
            Scalar untrackedShare = 0.0;
            for (std::size_t i = 0; i < ions.size(); ++i) {
                const Scalar share
                    = regionTotal > 0.0 ? region[static_cast<SaltIndex>(i)] / regionTotal : 0.0;
                ions[static_cast<SaltIndex>(i)] = share;
                if (!tracked_[i]) {
                    untrackedShare += share;
                }
            }

            Scalar trackedSum = 0.0;
            for (const auto& tracer : tracers_) {
                trackedSum
                    += std::max(Scalar(tracerConcentration(tracer.index, cell)), Scalar(0.0));
            }
            const Scalar trackedShare = std::max(Scalar(1.0) - untrackedShare, Scalar(0.0));
            if (trackedSum > 0.0) {
                for (const auto& tracer : tracers_) {
                    const Scalar value
                        = std::max(Scalar(tracerConcentration(tracer.index, cell)), Scalar(0.0));
                    ions[tracer.ion] = trackedShare * value / trackedSum;
                }
            }

            // Pairing scales with total salt, so any positive scale gives the same fractions
            const Scalar scale = regionTotal > 0.0 ? regionTotal : defaultScale_;
            for (std::size_t i = 0; i < ions.size(); ++i) {
                ions[static_cast<SaltIndex>(i)] *= scale;
            }
            fractions_[cell] = pvt.cellFractions(ions);

            const std::optional<Scalar> salt = saltConcentration(cell);
            if (salt && *salt > minSalt_ && trackedShare > 0.0) {
                const Scalar expected = *salt * trackedShare;
                maxMismatch_ = std::max(maxMismatch_, std::abs(trackedSum - expected) / expected);
            }
        }
    }

    /// The cell's fractions, or nullptr before the first update
    const ElectrolyteFractions* composition(std::size_t cell) const
    {
        return fractions_.empty() ? nullptr : &fractions_[cell];
    }

    /// Largest relative difference between the ion tracer sum and the BRINE total
    Scalar maxMismatch() const
    {
        return maxMismatch_;
    }

private:
    struct IonTracer {
        int index;
        SaltIndex ion;
    };

    // Below this the mismatch is not meaningful [kg/sm3]
    static constexpr Scalar minSalt_ = 1e-3;
    static constexpr Scalar defaultScale_ = 0.1;

    std::vector<IonTracer> tracers_ {};
    std::array<bool, numSaltIons> tracked_ {};
    std::vector<ElectrolyteFractions> fractions_ {};
    Scalar maxMismatch_ {0.0};
};

} // namespace Opm

#endif
