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

#include <config.h>

#define BOOST_TEST_MODULE BrineTracerComposition
#include <boost/test/unit_test.hpp>

#include <opm/simulators/flow/BrineTracerComposition.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace
{

using Composition = Opm::BrineTracerComposition<double>;
using Pvt = Composition::Pvt;
using Opm::SaltIndex;

// Region 1: NaCl-CaCl2-LiCl brine, charge balanced
Pvt
makePvt()
{
    Opm::SaltArray<double, Opm::SaltMolality> molality;
    molality[SaltIndex::NA] = 2.0;
    molality[SaltIndex::CA] = 0.4;
    molality[SaltIndex::LI] = 0.1;
    molality[SaltIndex::CL] = 2.9;
    Pvt pvt;
    pvt.setNumRegions(1);
    pvt.setSalinity(0, molality.convert_to<Opm::SaltMassFraction>());
    pvt.setEnableSaltConcentration(true);
    pvt.initEnd();
    return pvt;
}

double
share(const Pvt& pvt, SaltIndex ion)
{
    return pvt.salinity(0)[ion] / pvt.salinity(0).sum();
}

void
checkSame(const Opm::ElectrolyteFractions& a, const Opm::ElectrolyteFractions& b)
{
    for (std::size_t i = 0; i < a.size(); ++i) {
        BOOST_CHECK_SMALL(double(a[i]) - double(b[i]), 1e-6);
    }
}

} // namespace

BOOST_AUTO_TEST_CASE(FindsIonTracers)
{
    Composition composition;
    BOOST_CHECK(!composition.active());
    BOOST_CHECK(composition.composition(0) == nullptr);

    // Non-ion names and gas tracers are not tracked
    BOOST_CHECK(!composition.init({"DYE", "LI"}, {true, false}));
    BOOST_CHECK(!composition.active());

    BOOST_CHECK(composition.init({"DYE", "LI", "NA"}, {true, true, true}));
    BOOST_CHECK(composition.active());
}

BOOST_AUTO_TEST_CASE(TracerRatios)
{
    const auto pvt = makePvt();
    Composition composition;
    composition.init({"LI", "NA"}, {true, true});

    // Cell 0: no tracer, so SALINITC ratios. Cell 1: lithium-rich water.
    const std::vector<std::vector<double>> tracer {{0.0, 6.0}, {0.0, 30.0}};
    composition.update(
        pvt,
        2,
        [&tracer](int t, std::size_t cell) { return tracer[t][cell]; },
        [](std::size_t) { return 0u; },
        [](std::size_t) { return std::optional<double> {}; });

    checkSame(*composition.composition(0), pvt.cellFractions(pvt.salinity(0)));

    // Ca and Cl keep their SALINITC share; Li and Na split the rest 6:30
    const double untracked = share(pvt, SaltIndex::CA) + share(pvt, SaltIndex::CL);
    Opm::SaltArray<double, Opm::SaltMassFraction> ions;
    ions[SaltIndex::CA] = share(pvt, SaltIndex::CA);
    ions[SaltIndex::CL] = share(pvt, SaltIndex::CL);
    ions[SaltIndex::LI] = (1.0 - untracked) * 6.0 / 36.0;
    ions[SaltIndex::NA] = (1.0 - untracked) * 30.0 / 36.0;
    // As mass fractions of a brine with the region's total salt
    for (std::size_t i = 0; i < ions.size(); ++i) {
        ions[static_cast<SaltIndex>(i)] *= pvt.salinity(0).sum();
    }
    checkSame(*composition.composition(1), pvt.cellFractions(ions));

    // More lithium than the region brine
    const auto lithium = static_cast<std::size_t>(Pvt::Electrolyte::LiCl);
    BOOST_CHECK((*composition.composition(1))[lithium] > (*composition.composition(0))[lithium]);
}

BOOST_AUTO_TEST_CASE(SameRatiosAsRegion)
{
    // Tracers in the SALINITC proportions give the region's fractions
    const auto pvt = makePvt();
    Composition composition;
    composition.init({"NA", "CA", "LI", "CL"}, {true, true, true, true});

    const double c = 150.0;
    const auto& region = pvt.salinity(0);
    composition.update(
        pvt,
        1,
        [&](int t, std::size_t) {
            constexpr std::array ion {SaltIndex::NA, SaltIndex::CA, SaltIndex::LI, SaltIndex::CL};
            return c * region[ion[t]] / region.sum();
        },
        [](std::size_t) { return 0u; },
        [c](std::size_t) { return std::optional<double> {c}; });

    checkSame(*composition.composition(0), pvt.cellFractions(region));
    BOOST_CHECK_SMALL(composition.maxMismatch(), 1e-12);
}

BOOST_AUTO_TEST_CASE(Mismatch)
{
    const auto pvt = makePvt();
    Composition composition;
    composition.init({"LI", "NA"}, {true, true});

    // The tracked ions should hold c times their share; give them 10% more
    const double c = 200.0;
    const double trackedShare = share(pvt, SaltIndex::LI) + share(pvt, SaltIndex::NA);
    const double sum = 1.1 * c * trackedShare;
    composition.update(
        pvt,
        2,
        [sum](int t, std::size_t) { return t == 0 ? 0.1 * sum : 0.9 * sum; },
        [](std::size_t) { return 0u; },
        // Cell 1 holds precipitated salt, so it is not checked
        [c](std::size_t cell) { return cell == 0 ? std::optional<double> {c} : std::nullopt; });

    BOOST_CHECK_CLOSE(composition.maxMismatch(), 0.1, 1e-8);
}
