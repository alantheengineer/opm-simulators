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

#include <flow/flow_onephase_brine.hpp>

#include <opm/material/common/ResetLocale.hpp>

#include <opm/models/blackoil/blackoilbrinemodules.hh>
#include <opm/models/blackoil/blackoilconvectivemixingmodule.hh>
#include <opm/models/blackoil/blackoildiffusionmodule.hh>
#include <opm/models/blackoil/blackoillocalresidualtpfa.hh>
#include <opm/models/blackoil/blackoilonephaseindices.hh>

#include <opm/models/discretization/common/tpfalinearizer.hh>

#include <opm/simulators/flow/Main.hpp>

#include <memory>

namespace Opm::Properties
{

namespace TTag
{

    struct FlowWaterOnlyBrineProblem {
        using InheritsFrom = std::tuple<FlowProblem>;
    };

} // namespace TTag

template <class TypeTag>
struct EnableBrine<TypeTag, TTag::FlowWaterOnlyBrineProblem> {
    static constexpr bool value = true;
};

template <class TypeTag>
struct Linearizer<TypeTag, TTag::FlowWaterOnlyBrineProblem> {
    using type = TpfaLinearizer<TypeTag>;
};

template <class TypeTag>
struct LocalResidual<TypeTag, TTag::FlowWaterOnlyBrineProblem> {
    using type = BlackOilLocalResidualTPFA<TypeTag>;
};

// Water is the only phase; brine adds the salt concentration
template <class TypeTag>
struct Indices<TypeTag, TTag::FlowWaterOnlyBrineProblem> {
private:
    // TypeTag here would make the property definitions cyclic
    using BaseTypeTag = TTag::FlowProblem;
    using FluidSystem = GetPropType<BaseTypeTag, Properties::FluidSystem>;

public:
    using type = BlackOilOnePhaseIndices<getPropValue<TypeTag, Properties::EnableSolvent>(),
                                         getPropValue<TypeTag, Properties::EnableExtbo>(),
                                         getPropValue<TypeTag, Properties::EnablePolymer>(),
                                         /*numEnergyVars=*/0,
                                         getPropValue<TypeTag, Properties::EnableFoam>(),
                                         getPropValue<TypeTag, Properties::EnableBrine>(),
                                         /*PVOffset=*/0,
                                         /*enabledCompIdx=*/FluidSystem::waterCompIdx,
                                         getPropValue<TypeTag, Properties::EnableBioeffects>()>;
};

} // namespace Opm::Properties

namespace Opm
{

int
flowWaterOnlyBrineMain(int argc, char** argv, bool outputCout, bool outputFiles)
{
    // Use the default locale, so local settings cannot change number parsing
    resetLocale();

    FlowMain<Properties::TTag::FlowWaterOnlyBrineProblem> mainfunc {
        argc, argv, outputCout, outputFiles};
    return mainfunc.execute();
}

int
flowWaterOnlyBrineMainStandalone(int argc, char** argv)
{
    using TypeTag = Properties::TTag::FlowWaterOnlyBrineProblem;
    auto mainObject = std::make_unique<Opm::Main>(argc, argv);
    auto ret = mainObject->runStatic<TypeTag>();
    // Destroy mainObject first, as its destructor calls MPI_Finalize
    mainObject.reset();
    return ret;
}

} // namespace Opm
