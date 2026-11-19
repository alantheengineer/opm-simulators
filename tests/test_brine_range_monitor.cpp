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

#define BOOST_TEST_MODULE BrineRangeMonitor
#include <boost/test/unit_test.hpp>

#include <opm/simulators/flow/BrineRangeMonitor.hpp>

#include <dune/common/parallel/communication.hh>

#include <string>

namespace
{

using Monitor = Opm::BrineRangeMonitor<double>;
using Pvt = Monitor::Pvt;
using Policy = Monitor::Policy;
using Opm::SaltIndex;

// NaCl and CaCl2 brine: density fitted to 523 K and viscosity to 575 K, both to 60 MPa
Pvt
makePvt()
{
    Opm::SaltArray<double, Opm::SaltMolality> molality;
    molality[SaltIndex::NA] = 2.0;
    molality[SaltIndex::CA] = 0.4;
    molality[SaltIndex::CL] = 2.8;
    Pvt pvt;
    pvt.setNumRegions(1);
    pvt.setSalinity(0, molality.convert_to<Opm::SaltMassFraction>());
    pvt.setEnableSaltConcentration(true);
    pvt.initEnd();
    return pvt;
}

double
regionSalt(const Pvt& pvt)
{
    return pvt.salinity(0).sum() * pvt.waterReferenceDensity(0);
}

bool
contains(const std::string& text, const std::string& part)
{
    return text.find(part) != std::string::npos;
}

const Dune::Communication<Dune::No_Comm> serial {};

constexpr double pRes = 3.0e7;

} // namespace

BOOST_AUTO_TEST_CASE(InRangeIsQuiet)
{
    const auto pvt = makePvt();
    Monitor monitor;
    monitor.init(pvt, Policy::Warn, 2);
    monitor.check(0, 0, 360.0, pRes, regionSalt(pvt), nullptr);
    monitor.check(1, 0, 400.0, pRes, regionSalt(pvt), nullptr);
    const auto report = monitor.endReportStep(serial, 1);
    BOOST_CHECK(report.messages.empty());
    BOOST_CHECK(!report.stop);
}

BOOST_AUTO_TEST_CASE(WarnOncePerRegionAndProperty)
{
    const auto pvt = makePvt();
    Monitor monitor;
    monitor.init(pvt, Policy::Warn, 3);
    const double c = regionSalt(pvt);

    // Two cells above the density range, one checked twice in the step
    monitor.check(0, 0, 540.0, pRes, c, nullptr);
    monitor.check(0, 0, 545.0, pRes, c, nullptr);
    monitor.check(1, 0, 530.0, pRes, c, nullptr);
    monitor.check(2, 0, 360.0, pRes, c, nullptr);
    auto report = monitor.endReportStep(serial, 4);
    BOOST_REQUIRE_EQUAL(report.messages.size(), 1U);
    const auto& message = report.messages.front();
    BOOST_CHECK(contains(message, "brine density in PVT region 1"));
    BOOST_CHECK(contains(message, "in 2 cells in report step 4"));
    BOOST_CHECK(contains(message, "temperature up to 271.9 C (fitted to"));
    BOOST_CHECK(!contains(message, "pressure"));
    BOOST_CHECK(!report.stop);

    // The same excursion again is not repeated, but viscosity is new
    monitor.check(0, 0, 545.0, pRes, c, nullptr);
    monitor.check(1, 0, 360.0, 70e6, c, nullptr);
    report = monitor.endReportStep(serial, 5);
    BOOST_REQUIRE_EQUAL(report.messages.size(), 1U);
    BOOST_CHECK(contains(report.messages.front(), "brine viscosity in PVT region 1"));
    BOOST_CHECK(contains(report.messages.front(), "pressure up to 70.0 MPa (fitted to 60.0 MPa)"));
}

BOOST_AUTO_TEST_CASE(ErrorStopsEveryTime)
{
    const auto pvt = makePvt();
    Monitor monitor;
    monitor.init(pvt, Policy::Error, 1);
    const double c = regionSalt(pvt);

    monitor.check(0, 0, 265.0, pRes, c, nullptr);
    auto report = monitor.endReportStep(serial, 1);
    BOOST_CHECK_EQUAL(report.messages.size(), 2U);
    BOOST_CHECK(contains(report.messages.front(), "temperature down to -8.1 C"));
    BOOST_CHECK(contains(report.messages.front(), "RANGE_POLICY is ERROR"));
    BOOST_CHECK(report.stop);

    monitor.check(0, 0, 265.0, pRes, c, nullptr);
    report = monitor.endReportStep(serial, 2);
    BOOST_CHECK(report.stop);

    // Counts and extremes reset with the report step
    report = monitor.endReportStep(serial, 3);
    BOOST_CHECK(report.messages.empty());
    BOOST_CHECK(!report.stop);
}

BOOST_AUTO_TEST_CASE(SalinityBeyondFit)
{
    const auto pvt = makePvt();
    Monitor monitor;
    monitor.init(pvt, Policy::Clamp, 1);
    monitor.check(0, 0, 360.0, pRes, 2.5 * regionSalt(pvt), nullptr);
    const auto report = monitor.endReportStep(serial, 1);
    BOOST_REQUIRE_EQUAL(report.messages.size(), 2U);
    BOOST_CHECK(contains(report.messages.front(), "ionic strength up to"));
    BOOST_CHECK(contains(report.messages.front(), "(fitted to 6.00 mol/kg)"));
    BOOST_CHECK(contains(report.messages.front(), "held at the range edge"));
    BOOST_CHECK(!report.stop);
}
