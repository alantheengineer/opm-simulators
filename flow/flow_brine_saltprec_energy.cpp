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

#include "config.h"

#include <flow/flow_brine_saltprec_energy.hpp>

#include <opm/material/common/ResetLocale.hpp>
#include <opm/material/thermal/EnergyModuleType.hpp>

#include <opm/models/blackoil/blackoilbrinemodules.hh>
#include <opm/models/blackoil/blackoilconvectivemixingmodule.hh>
#include <opm/models/blackoil/blackoildiffusionmodule.hh>
#include <opm/models/blackoil/blackoilenergymodules.hh>
#include <opm/models/blackoil/blackoillocalresidualtpfa.hh>

#include <opm/models/discretization/common/tpfalinearizer.hh>

#include <opm/simulators/flow/Main.hpp>

#include <memory>

namespace Opm::Properties
{

namespace TTag
{

    struct FlowBrineSaltprecEnergyProblem {
        using InheritsFrom = std::tuple<FlowProblem>;
    };

} // namespace TTag

template <class TypeTag>
struct EnableBrine<TypeTag, TTag::FlowBrineSaltprecEnergyProblem> {
    static constexpr bool value = true;
};

template <class TypeTag>
struct EnableSaltPrecipitation<TypeTag, TTag::FlowBrineSaltprecEnergyProblem> {
    static constexpr bool value = true;
};

template <class TypeTag>
struct Linearizer<TypeTag, TTag::FlowBrineSaltprecEnergyProblem> {
    using type = TpfaLinearizer<TypeTag>;
};

template <class TypeTag>
struct LocalResidual<TypeTag, TTag::FlowBrineSaltprecEnergyProblem> {
    using type = BlackOilLocalResidualTPFA<TypeTag>;
};

template <class TypeTag>
struct EnergyModuleType<TypeTag, TTag::FlowBrineSaltprecEnergyProblem> {
    static constexpr EnergyModules value = EnergyModules::FullyImplicitThermal;
};

} // namespace Opm::Properties

namespace Opm
{

int
flowBrineSaltprecEnergyMain(int argc, char** argv, bool outputCout, bool outputFiles)
{
    // Use the default locale, so local settings cannot change number parsing
    resetLocale();

    FlowMain<Properties::TTag::FlowBrineSaltprecEnergyProblem> mainfunc {
        argc, argv, outputCout, outputFiles};
    return mainfunc.execute();
}

int
flowBrineSaltprecEnergyMainStandalone(int argc, char** argv)
{
    using TypeTag = Properties::TTag::FlowBrineSaltprecEnergyProblem;
    auto mainObject = std::make_unique<Opm::Main>(argc, argv);
    auto ret = mainObject->runStatic<TypeTag>();
    // Destroy mainObject first, as its destructor calls MPI_Finalize
    mainObject.reset();
    return ret;
}

} // namespace Opm
