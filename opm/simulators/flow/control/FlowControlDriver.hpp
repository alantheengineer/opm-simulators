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

#include <dune/grid/common/partitionset.hh>
#include <dune/grid/common/rangegenerators.hh>

#include <opm/common/utility/TimeService.hpp>

#include <opm/input/eclipse/Deck/Deck.hpp>
#include <opm/input/eclipse/Deck/DeckKeyword.hpp>
#include <opm/input/eclipse/EclipseState/EclipseState.hpp>
#include <opm/input/eclipse/EclipseState/IOConfig/IOConfig.hpp>
#include <opm/input/eclipse/Parser/ErrorGuard.hpp>
#include <opm/input/eclipse/Parser/InputErrorAction.hpp>
#include <opm/input/eclipse/Parser/ParseContext.hpp>
#include <opm/input/eclipse/Parser/Parser.hpp>
#include <opm/input/eclipse/Schedule/Action/SimulatorUpdate.hpp>
#include <opm/input/eclipse/Schedule/Schedule.hpp>
#include <opm/input/eclipse/Schedule/ScheduleTypes.hpp>
#include <opm/input/eclipse/Schedule/SummaryState.hpp>
#include <opm/input/eclipse/Schedule/Well/Connection.hpp>
#include <opm/input/eclipse/Schedule/Well/Well.hpp>
#include <opm/input/eclipse/Schedule/Well/WellConnections.hpp>
#include <opm/input/eclipse/Schedule/Well/WellEnums.hpp>
#include <opm/input/eclipse/Schedule/Well/WellInjectionControls.hpp>
#include <opm/input/eclipse/Schedule/Well/WellProductionControls.hpp>
#include <opm/input/eclipse/Units/UnitSystem.hpp>

#include <opm/material/densead/Math.hpp>

#include <opm/models/utils/propertysystem.hh>

#include <opm/output/data/Wells.hpp>

#include <opm/simulators/flow/control/ControlTypes.hpp>
#include <opm/simulators/utils/moduleVersion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_OPENMP) && _OPENMP
#include <omp.h>
#endif

namespace Opm
{
template <class TypeTag>
class FlowMain;
template <class TypeTag>
class FlowProblemBlackoil;
} // namespace Opm

namespace Opm::Control
{

// Control mode reads blackoil state, so it is offered for the variants that
// use flow's blackoil problem.
template <class TypeTag>
constexpr bool controlSupported
    = std::is_base_of_v<FlowProblemBlackoil<TypeTag>, GetPropType<TypeTag, Properties::Problem>>;

// Adapts FlowMain to the driver interface of runControlLoop(), using only
// its public step API. Valid after FlowMain has run the init step.
template <class TypeTag>
class FlowControlDriver
{
    using Main = FlowMain<TypeTag>;
    using FluidSystem = GetPropType<TypeTag, Properties::FluidSystem>;
    using Measure = UnitSystem::measure;

    enum class Field {
        Pressure,
        Swat,
        Sgas,
        Soil,
        Rs,
        Rv,
        Temp,
        Salt,
        Poro,
        Porv,
        PermX,
        PermY,
        PermZ,
        Depth,
        KrWat,
        KrOil,
        KrGas,
        ViscWat,
        ViscOil,
        ViscGas,
        DenWat,
        DenOil,
        DenGas,
        Bw,
        Bo,
        Bg,
        CompWat,
        CompOil,
        CompGas,
        CompRock
    };

    struct FieldInfo {
        std::string_view name;
        Field field;
        Measure measure;
        std::string_view siUnit;
        // Per unit of pressure, so converted with the inverse of the pressure unit.
        bool perPressure = false;
    };

    static constexpr std::array<FieldInfo, 30> fields_ {{
        {"PRESSURE", Field::Pressure, Measure::pressure, "PA"},
        {"SWAT", Field::Swat, Measure::identity, ""},
        {"SGAS", Field::Sgas, Measure::identity, ""},
        {"SOIL", Field::Soil, Measure::identity, ""},
        {"RS", Field::Rs, Measure::gas_oil_ratio, "SM3/SM3"},
        {"RV", Field::Rv, Measure::oil_gas_ratio, "SM3/SM3"},
        {"TEMP", Field::Temp, Measure::temperature, "K"},
        {"SALT", Field::Salt, Measure::concentration, "KG/M3"},
        {"PORO", Field::Poro, Measure::identity, ""},
        {"PORV", Field::Porv, Measure::volume, "M3"},
        {"PERMX", Field::PermX, Measure::permeability, "M2"},
        {"PERMY", Field::PermY, Measure::permeability, "M2"},
        {"PERMZ", Field::PermZ, Measure::permeability, "M2"},
        {"DEPTH", Field::Depth, Measure::length, "M"},
        {"WATKR", Field::KrWat, Measure::identity, ""},
        {"OILKR", Field::KrOil, Measure::identity, ""},
        {"GASKR", Field::KrGas, Measure::identity, ""},
        {"WAT_VISC", Field::ViscWat, Measure::viscosity, "PA*S"},
        {"OIL_VISC", Field::ViscOil, Measure::viscosity, "PA*S"},
        {"GAS_VISC", Field::ViscGas, Measure::viscosity, "PA*S"},
        {"WAT_DEN", Field::DenWat, Measure::density, "KG/M3"},
        {"OIL_DEN", Field::DenOil, Measure::density, "KG/M3"},
        {"GAS_DEN", Field::DenGas, Measure::density, "KG/M3"},
        {"BW", Field::Bw, Measure::water_formation_volume_factor, "RM3/SM3"},
        {"BO", Field::Bo, Measure::oil_formation_volume_factor, "RM3/SM3"},
        {"BG", Field::Bg, Measure::gas_formation_volume_factor, "RM3/SM3"},
        {"WAT_COMP", Field::CompWat, Measure::pressure, "1/PA", true},
        {"OIL_COMP", Field::CompOil, Measure::pressure, "1/PA", true},
        {"GAS_COMP", Field::CompGas, Measure::pressure, "1/PA", true},
        {"ROCK_COMP", Field::CompRock, Measure::pressure, "1/PA", true},
    }};

public:
    explicit FlowControlDriver(Main& main)
        : main_(main)
    {
    }

    // Collective. Also builds the map from local cells to active indices.
    Info info()
    {
        auto& simulator = *main_.getSimulatorPtr();
        const auto& vanguard = simulator.vanguard();
        const auto& schedule = vanguard.schedule();

        buildCellMap();

        Info info;
        info.engineVersion = moduleVersion();
        info.ranks = vanguard.grid().comm().size();
#if defined(_OPENMP) && _OPENMP
        info.threadsPerRank = omp_get_max_threads();
#else
        info.threadsPerRank = 1;
#endif
        info.gridDims = dims_;
        info.activeCells = activeCells_;
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
        const auto& report = main_.getStepDriverPtr()->report();
        const auto success = report.success;
        const auto failure = report.failure;
        const std::size_t first = report.stepreports.size();

        stepped_ = true;
        // False when the schedule asks the run to stop, for example EXIT.
        if (main_.executeStep() == 0) {
            stopped_ = true;
        }

        lastStep_ = StepReport {};
        for (std::size_t i = first; i < report.stepreports.size(); ++i) {
            if (report.stepreports[i].converged) {
                ++lastStep_.timeSteps;
            } else {
                ++lastStep_.cuts;
            }
        }
        lastStep_.newtonIterations
            = difference(report.success.total_newton_iterations, success.total_newton_iterations);
        lastStep_.linearIterations
            = difference(report.success.total_linear_iterations, success.total_linear_iterations);
        lastStep_.wastedNewtonIterations
            = difference(report.failure.total_newton_iterations, failure.total_newton_iterations);
        recordSummary();
    }

    StepReport lastStep() const
    {
        return lastStep_;
    }

    int finish()
    {
        // The timing trailer of executeStepsCleanup() needs the solver, which
        // flow creates in the first report step.
        if (!stepped_) {
            return main_.getStepDriverPtr()->finalize().success.exit_status;
        }
        return main_.executeStepsCleanup();
    }

    // Collective. The result is on rank 0 only.
    CellData
    cellData(const std::vector<std::string>& names, bool siUnits, const CellSelection& selection)
    {
        std::vector<const FieldInfo*> requested;
        for (const auto& name : names) {
            requested.push_back(&lookup(name));
        }

        auto& simulator = *main_.getSimulatorPtr();
        const auto& comm = simulator.vanguard().grid().comm();
        const auto& units = simulator.vanguard().eclState().getUnits();

        // Every rank must reach each gather, so a missing value is reported
        // after agreeing on it.
        CellData data;
        for (const FieldInfo* info : requested) {
            std::vector<double> local(localCells_.size());
            bool ok = true;
            for (std::size_t i = 0; i < localCells_.size() && ok; ++i) {
                const auto value = cellValue(simulator, info->field, localCells_[i]);
                ok = value.has_value();
                local[i] = value.value_or(0.0);
            }
            if (comm.min(ok ? 1 : 0) == 0) {
                throw BadRequest("Cell values are not available yet");
            }
            auto values = gather(comm, local);
            if (comm.rank() != 0) {
                continue;
            }
            CellField field;
            field.name = info->name;
            if (siUnits) {
                field.unit = info->siUnit;
            } else if (info->perPressure) {
                const double perUnit = units.from_si(Measure::pressure, 1.0);
                for (auto& value : values) {
                    value /= perUnit;
                }
                field.unit = "1/" + std::string(units.name(Measure::pressure));
            } else {
                for (auto& value : values) {
                    value = units.from_si(info->measure, value);
                }
                field.unit = units.name(info->measure);
            }
            field.values = std::move(values);
            data.fields.push_back(std::move(field));
        }
        if (comm.rank() == 0) {
            select(selection, data);
        }
        return data;
    }

    // Rank 0 only; the other ranks return nothing.
    WellData wellData(const std::vector<std::string>& names, bool withConnections)
    {
        auto& simulator = *main_.getSimulatorPtr();
        if (simulator.vanguard().grid().comm().rank() != 0) {
            return {};
        }
        auto& problem = simulator.problem();
        const auto& collect = problem.eclWriter().collectOnIORank();
        const data::Wells wells
            = collect.isParallel() ? collect.globalWellData() : problem.wellModel().wellData();
        const auto& schedule = simulator.vanguard().schedule();
        const auto& summaryState = simulator.vanguard().summaryState();
        // The wells and limits in force for the next report step to run.
        const auto step = static_cast<std::size_t>(std::min(reportStep(), reportStepsTotal() - 1));
        auto wellNames = schedule.wellNames(step);
        std::sort(wellNames.begin(), wellNames.end());

        for (const auto& name : names) {
            const bool found
                = std::any_of(wellNames.begin(), wellNames.end(), [&name](const auto& well) {
                      return matches(name, well);
                  });
            if (!found) {
                throw BadRequest("No well matches " + name);
            }
        }

        const auto& units = simulator.vanguard().eclState().getUnits();
        using Rate = data::Rates::opt;
        WellData data;
        data.liquidRateUnit = units.name(Measure::liquid_surface_rate);
        data.gasRateUnit = units.name(Measure::gas_surface_rate);
        data.pressureUnit = units.name(Measure::pressure);
        data.temperatureUnit = units.name(Measure::temperature);
        data.saltRateUnit = units.name(Measure::mass_rate);
        data.transmissibilityUnit = units.name(Measure::transmissibility);
        data.khUnit = units.name(Measure::effective_Kh);
        data.lengthUnit = units.name(Measure::length);
        data.reservoirRateUnit = units.name(Measure::rate);
        data.liquidProductivityIndexUnit = units.name(Measure::liquid_productivity_index);
        data.gasProductivityIndexUnit = units.name(Measure::gas_productivity_index);
        for (const auto& name : wellNames) {
            const bool wanted
                = names.empty() || std::any_of(names.begin(), names.end(), [&name](const auto& p) {
                      return matches(p, name);
                  });
            if (!wanted) {
                continue;
            }
            const auto& deckWell = schedule.getWell(name, step);
            WellValues values;
            values.name = name;
            values.injector = deckWell.isInjector();
            if (values.injector) {
                const auto controls = deckWell.injectionControls(summaryState);
                values.injectorType = InjectorType2String(deckWell.injectorType());
                values.bhpLimit = units.from_si(Measure::pressure, controls.bhp_limit);
                values.control = modeName(controls.cmode);
            } else {
                const auto controls = deckWell.productionControls(summaryState);
                values.bhpLimit = units.from_si(Measure::pressure, controls.bhp_limit);
                values.control = modeName(controls.cmode);
            }
            values.status = WellStatus2String(deckWell.getStatus());

            // Before the first step there is no solution, and the deck says
            // how the well starts.
            const auto it = wells.find(name);
            if (withConnections) {
                values.connections
                    = connectionValues(deckWell, it == wells.end() ? nullptr : &it->second, units);
            }
            if (it == wells.end()) {
                data.wells.push_back(std::move(values));
                continue;
            }
            const auto& well = it->second;
            values.status = WellStatus2String(well.dynamicStatus);
            values.control = controlName(well.current_control);
            values.oilRate
                = units.from_si(Measure::liquid_surface_rate, well.rates.get(Rate::oil, 0.0));
            values.waterRate
                = units.from_si(Measure::liquid_surface_rate, well.rates.get(Rate::wat, 0.0));
            values.gasRate
                = units.from_si(Measure::gas_surface_rate, well.rates.get(Rate::gas, 0.0));
            values.bhp = units.from_si(Measure::pressure, well.bhp);
            values.thp = units.from_si(Measure::pressure, well.thp);
            values.temperature = units.from_si(Measure::temperature, well.temperature);
            values.saltRate = units.from_si(Measure::mass_rate, well.rates.get(Rate::brine, 0.0));
            data.wells.push_back(std::move(values));
        }
        return data;
    }

    // Rank 0 only; the other ranks return nothing.
    SummaryData summary(const std::vector<std::string>& keys, bool history) const
    {
        auto& simulator = *main_.getSimulatorPtr();
        if (simulator.vanguard().grid().comm().rank() != 0) {
            return {};
        }

        std::vector<std::string> selected = keys;
        if (selected.empty()) {
            selected = summaryKeys_;
            std::sort(selected.begin(), selected.end());
        }
        for (const auto& key : selected) {
            if (summaryColumn_.find(key) == summaryColumn_.end() && !hasSummaryKey(key)) {
                throw BadRequest("Unknown summary key " + key);
            }
        }

        SummaryData data;
        const std::size_t first = history || summarySteps_.empty() ? 0 : summarySteps_.size() - 1;
        data.reportSteps.assign(summarySteps_.begin() + static_cast<std::ptrdiff_t>(first),
                                summarySteps_.end());
        data.elapsedSeconds.assign(summaryElapsed_.begin() + static_cast<std::ptrdiff_t>(first),
                                   summaryElapsed_.end());
        for (const auto& key : selected) {
            // NaN where the key had no value yet.
            const auto it = summaryColumn_.find(key);
            SummarySeries series;
            series.key = key;
            for (std::size_t row = first; row < summaryValues_.size(); ++row) {
                const auto& values = summaryValues_[row];
                const bool known = it != summaryColumn_.end() && it->second < values.size();
                series.values.push_back(known ? values[it->second]
                                              : std::numeric_limits<double>::quiet_NaN());
            }
            data.series.push_back(std::move(series));
        }
        return data;
    }

    // Every rank. Inserts the keywords at the start of the next report step,
    // as if written in the deck at that date. Throws BadRequest, with nothing
    // applied, if any keyword is not allowed or cannot be applied.
    ScheduleChange applySchedule(const std::string& text)
    {
        auto& vanguard = main_.getSimulatorPtr()->vanguard();
        auto& schedule = vanguard.schedule();
        const auto keywords = parseKeywords(text, vanguard.eclState().getDeckUnitSystem());

        ScheduleChange change;
        change.text = text;
        change.reportStep = reportStep();
        for (const auto& keyword : keywords) {
            if (!allowedKeywords().contains(keyword->name())) {
                throw BadRequest("Keyword " + keyword->name() + " is not allowed in control mode");
            }
            change.keywords.push_back(keyword->name());
        }

        // A trial on a copy, so that a keyword that fails leaves the run as it
        // was.
        const auto step = static_cast<std::size_t>(change.reportStep);
        try {
            Schedule trial(schedule);
            trial.insertKeywords(keywords, step);
        } catch (const std::exception& e) {
            throw BadRequest(e.what());
        }
        const auto update = schedule.insertKeywords(keywords, step);
        change.wells.assign(update.affected_wells.begin(), update.affected_wells.end());
        std::sort(change.wells.begin(), change.wells.end());
        return change;
    }

    // Every rank. Saves the state at the current report step to
    // CASE-LABEL.OPMRST in the output folder. The state is not changed, so a
    // failure to write is reported as a bad request.
    Checkpoint saveCheckpoint(const std::string& label)
    {
        const auto& ioConfig = main_.getSimulatorPtr()->vanguard().eclState().getIOConfig();
        Checkpoint checkpoint;
        checkpoint.file = (std::filesystem::path(ioConfig.getOutputDir())
                           / (ioConfig.getBaseName() + "-" + label + ".OPMRST"))
                              .string();
        checkpoint.reportStep = reportStep();
        try {
            main_.getStepDriverPtr()->saveCheckpoint(*main_.getSimTimer(), checkpoint.file);
        } catch (const std::exception& e) {
            throw BadRequest(std::string("Cannot save the checkpoint: ") + e.what());
        }
        return checkpoint;
    }

    // Rank 0 only. Whether the deck asks for the key, or flow evaluates it.
    bool hasSummaryKey(const std::string& key) const
    {
        const auto& vanguard = main_.getSimulatorPtr()->vanguard();
        return vanguard.summaryConfig().hasSummaryKey(key) || vanguard.summaryState().has(key);
    }

    // Rank 0 only. Latest value of a summary key, in SMSPEC units.
    std::optional<double> summaryValue(const std::string& key) const
    {
        const auto& state = main_.getSimulatorPtr()->vanguard().summaryState();
        if (!state.has(key)) {
            return std::nullopt;
        }
        return state.get(key);
    }

private:
    // Empty when the well has no active control mode, as for some shut or
    // history wells.
    static std::string modeName(WellInjectorCMode mode)
    {
        return mode == WellInjectorCMode::CMODE_UNDEFINED ? std::string {}
                                                          : WellInjectorCMode2String(mode);
    }

    static std::string modeName(WellProducerCMode mode)
    {
        return mode == WellProducerCMode::NONE || mode == WellProducerCMode::CMODE_UNDEFINED
            ? std::string {}
            : WellProducerCMode2String(mode);
    }

    static std::string controlName(const data::CurrentControl& control)
    {
        return control.isProducer ? modeName(control.prod) : modeName(control.inj);
    }

    // A well's connections in deck order, with the last step's values when
    // there is a solution. Rank 0.
    std::vector<ConnectionValues> connectionValues(const Well& deckWell,
                                                   const data::Well* solution,
                                                   const UnitSystem& units) const
    {
        using Rate = data::Rates::opt;
        std::vector<ConnectionValues> out;
        for (const auto& connection : deckWell.getConnections()) {
            ConnectionValues values;
            values.i = connection.getI() + 1;
            values.j = connection.getJ() + 1;
            values.k = connection.getK() + 1;
            values.cell = activeIndex(connection.global_index());
            values.status = Connection::State2String(connection.state());
            values.connectionFactor = units.from_si(Measure::transmissibility, connection.CF());
            values.kh = units.from_si(Measure::effective_Kh, connection.Kh());
            values.depth = units.from_si(Measure::length, connection.depth());
            const auto* dynamic = solution != nullptr
                ? solution->find_connection(connection.global_index())
                : nullptr;
            if (dynamic != nullptr) {
                const auto& rates = dynamic->rates;
                values.pressure = units.from_si(Measure::pressure, dynamic->pressure);
                values.oilRate
                    = units.from_si(Measure::liquid_surface_rate, rates.get(Rate::oil, 0.0));
                values.waterRate
                    = units.from_si(Measure::liquid_surface_rate, rates.get(Rate::wat, 0.0));
                values.gasRate
                    = units.from_si(Measure::gas_surface_rate, rates.get(Rate::gas, 0.0));
                values.reservoirRate = units.from_si(Measure::rate, dynamic->reservoir_rate);
                values.productivityIndexOil
                    = units.from_si(Measure::liquid_productivity_index,
                                    rates.get(Rate::productivity_index_oil, 0.0));
                values.productivityIndexWater
                    = units.from_si(Measure::liquid_productivity_index,
                                    rates.get(Rate::productivity_index_water, 0.0));
                values.productivityIndexGas = units.from_si(
                    Measure::gas_productivity_index, rates.get(Rate::productivity_index_gas, 0.0));
            }
            out.push_back(std::move(values));
        }
        return out;
    }

    // Active index of a Cartesian cell index, or -1 for an inactive cell.
    // Rank 0.
    std::int64_t activeIndex(std::size_t cartesian) const
    {
        const auto c = static_cast<int>(cartesian);
        const auto it = std::lower_bound(cartesianOf_.begin(), cartesianOf_.end(), c);
        return it != cartesianOf_.end() && *it == c ? std::distance(cartesianOf_.begin(), it) : -1;
    }

    static int difference(unsigned int after, unsigned int before)
    {
        return static_cast<int>(after - before);
    }

    // Keywords that only change controls, limits, injected composition and
    // transmissibility multipliers, which flow reads again at the start of
    // each report step.
    static const std::unordered_set<std::string>& allowedKeywords()
    {
        static const std::unordered_set<std::string> keywords {
            "BOX",      "ENDBOX",   "GCONINJE", "GCONPROD", "GEFAC",    "MULTX",    "MULTX-",
            "MULTY",    "MULTY-",   "MULTZ",    "MULTZ-",   "NEXTSTEP", "WCONHIST", "WCONINJE",
            "WCONINJH", "WCONPROD", "WECON",    "WEFAC",    "WELOPEN",  "WELTARG",  "WINJMULT",
            "WPIMULT",  "WSALT",    "WTEMP",    "WTMULT",   "WTRACER",
        };
        return keywords;
    }

    // Parses SCHEDULE text in the deck's units. Input errors throw rather than
    // being collected, so the error guard never ends the program.
    static std::vector<std::unique_ptr<DeckKeyword>> parseKeywords(const std::string& text,
                                                                   const UnitSystem& units)
    {
        Deck deck;
        ErrorGuard errors;
        try {
            ParseContext context(InputErrorAction::THROW_EXCEPTION);
            Parser parser;
            parser.silent(true);
            deck = parser.parseString(units.deck_name() + "\n\n" + text, context, errors);
        } catch (const std::exception& e) {
            errors.clear();
            throw BadRequest(std::string("Cannot parse the keywords: ") + e.what());
        }
        if (errors) {
            const auto message = errors.formattedErrors();
            errors.clear();
            throw BadRequest("Cannot parse the keywords: " + message);
        }

        std::vector<std::unique_ptr<DeckKeyword>> keywords;
        for (std::size_t i = 1; i < deck.size(); ++i) {
            keywords.push_back(std::make_unique<DeckKeyword>(deck[i]));
        }
        if (keywords.empty()) {
            throw BadRequest("No keywords given");
        }
        return keywords;
    }

    static bool matches(std::string_view pattern, std::string_view name)
    {
        if (!pattern.empty() && pattern.back() == '*') {
            return name.starts_with(pattern.substr(0, pattern.size() - 1));
        }
        return pattern == name;
    }

    static const FieldInfo& lookup(std::string_view name)
    {
        const auto it = std::find_if(
            fields_.begin(), fields_.end(), [name](const auto& f) { return f.name == name; });
        if (it == fields_.end()) {
            throw BadRequest("Unknown field " + std::string(name));
        }
        if (!available(it->field)) {
            throw BadRequest("Field " + std::string(name) + " is not in this model");
        }
        return *it;
    }

    static bool phase(unsigned phaseIdx)
    {
        return FluidSystem::phaseIsActive(phaseIdx);
    }

    static bool available(Field field)
    {
        switch (field) {
        case Field::Swat:
        case Field::KrWat:
        case Field::ViscWat:
        case Field::DenWat:
        case Field::Bw:
        case Field::CompWat:
            return phase(FluidSystem::waterPhaseIdx);
        case Field::Sgas:
        case Field::KrGas:
        case Field::ViscGas:
        case Field::DenGas:
        case Field::Bg:
        case Field::CompGas:
            return phase(FluidSystem::gasPhaseIdx);
        case Field::Soil:
        case Field::KrOil:
        case Field::ViscOil:
        case Field::DenOil:
        case Field::Bo:
        case Field::CompOil:
            return phase(FluidSystem::oilPhaseIdx);
        case Field::Rs:
        case Field::Rv:
            return phase(FluidSystem::oilPhaseIdx) && phase(FluidSystem::gasPhaseIdx);
        case Field::Salt:
            return hasSalt;
        default:
            return true;
        }
    }

    template <class Simulator>
    static std::optional<double> cellValue(Simulator& simulator, Field field, unsigned cell)
    {
        auto& problem = simulator.problem();
        switch (field) {
        case Field::Poro:
            return problem.referencePorosity(cell, 0);
        case Field::Porv:
            return problem.referencePorosity(cell, 0) * simulator.model().dofTotalVolume(cell);
        case Field::PermX:
            return problem.intrinsicPermeability(cell)[0][0];
        case Field::PermY:
            return problem.intrinsicPermeability(cell)[1][1];
        case Field::PermZ:
            return problem.intrinsicPermeability(cell)[2][2];
        case Field::Depth:
            return simulator.vanguard().cellCenterDepth(cell);
        case Field::CompRock:
            return problem.rockCompressibility(cell);
        default:
            break;
        }

        const auto* quantities = simulator.model().cachedIntensiveQuantities(cell, 0);
        if (quantities == nullptr) {
            return std::nullopt;
        }
        const auto& fs = quantities->fluidState();
        switch (field) {
        case Field::Pressure:
            // The phase the restart file reports.
            if (phase(FluidSystem::oilPhaseIdx)) {
                return getValue(fs.pressure(FluidSystem::oilPhaseIdx));
            }
            if (phase(FluidSystem::gasPhaseIdx)) {
                return getValue(fs.pressure(FluidSystem::gasPhaseIdx));
            }
            return getValue(fs.pressure(FluidSystem::waterPhaseIdx));
        case Field::Swat:
            return getValue(fs.saturation(FluidSystem::waterPhaseIdx));
        case Field::Sgas:
            return getValue(fs.saturation(FluidSystem::gasPhaseIdx));
        case Field::Soil:
            return getValue(fs.saturation(FluidSystem::oilPhaseIdx));
        case Field::Rs:
            return getValue(fs.Rs());
        case Field::Rv:
            return getValue(fs.Rv());
        case Field::Temp:
            return getValue(fs.temperature(0));
        case Field::Salt:
            if constexpr (hasSalt) {
                return getValue(fs.saltConcentration());
            }
            return std::nullopt;
        default:
            break;
        }

        const auto index = phaseOf(field);
        switch (field) {
        case Field::KrWat:
        case Field::KrOil:
        case Field::KrGas:
            return getValue(quantities->relativePermeability(index));
        case Field::ViscWat:
        case Field::ViscOil:
        case Field::ViscGas:
            return getValue(fs.viscosity(index));
        case Field::DenWat:
        case Field::DenOil:
        case Field::DenGas:
            return getValue(fs.density(index));
        case Field::Bw:
        case Field::Bo:
        case Field::Bg:
            return 1.0 / getValue(fs.invB(index));
        case Field::CompWat:
        case Field::CompOil:
        case Field::CompGas:
            return compressibility(fs, index);
        default:
            return std::nullopt;
        }
    }

    static unsigned phaseOf(Field field)
    {
        switch (field) {
        case Field::KrWat:
        case Field::ViscWat:
        case Field::DenWat:
        case Field::Bw:
        case Field::CompWat:
            return FluidSystem::waterPhaseIdx;
        case Field::KrGas:
        case Field::ViscGas:
        case Field::DenGas:
        case Field::Bg:
        case Field::CompGas:
            return FluidSystem::gasPhaseIdx;
        default:
            return FluidSystem::oilPhaseIdx;
        }
    }

    // -(1/B) dB/dp of one phase at constant composition (Rs, Rv, Rsw and salt
    // held), from the PVT tables by a small step up in pressure. Below the
    // bubble point this leaves out the gas that comes out of solution.
    template <class FluidState>
    static double compressibility(const FluidState& fs, unsigned phaseIdx)
    {
        const unsigned region = fs.pvtRegionIndex();
        const double temperature = getValue(fs.temperature(phaseIdx));
        const double pressure = getValue(fs.pressure(phaseIdx));
        const double step = std::max(1.0e-4 * pressure, 1.0e3);
        const auto invB = [&](double p) -> double {
            if (phaseIdx == FluidSystem::oilPhaseIdx) {
                return FluidSystem::oilPvt().inverseFormationVolumeFactor(
                    region, temperature, p, getValue(fs.Rs()));
            }
            if (phaseIdx == FluidSystem::gasPhaseIdx) {
                return FluidSystem::gasPvt().inverseFormationVolumeFactor(
                    region, temperature, p, getValue(fs.Rv()), getValue(fs.Rvw()));
            }
            return FluidSystem::waterPvt().inverseFormationVolumeFactor(
                region, temperature, p, getValue(fs.Rsw()), getValue(fs.saltConcentration()));
        };
        const double now = invB(pressure);
        return (invB(pressure + step) - now) / (step * now);
    }

    static constexpr bool hasSalt = getPropValue<TypeTag, Properties::EnableBrine>();

    // Interior cells of this rank and, on rank 0, where each one lands in
    // active cell order. Active cells are ordered by Cartesian index, as in
    // the INIT and restart files.
    void buildCellMap()
    {
        auto& simulator = *main_.getSimulatorPtr();
        const auto& vanguard = simulator.vanguard();
        const auto& comm = vanguard.grid().comm();
        const auto& mapper = simulator.model().elementMapper();

        std::vector<int> cartesian;
        for (const auto& element : elements(vanguard.gridView(), Dune::Partitions::interior)) {
            const auto cell = static_cast<unsigned>(mapper.index(element));
            localCells_.push_back(cell);
            cartesian.push_back(static_cast<int>(vanguard.cartesianIndex(cell)));
        }

        const auto& cartDims = vanguard.cartesianDimensions();
        for (std::size_t i = 0; i < dims_.size() && i < cartDims.size(); ++i) {
            dims_[i] = cartDims[i];
        }

        const auto size = static_cast<std::size_t>(comm.size());
        counts_.assign(size, 0);
        offsets_.assign(size, 0);
        int count = static_cast<int>(localCells_.size());
        comm.gather(&count, counts_.data(), 1, 0);
        std::partial_sum(counts_.begin(), counts_.end() - 1, offsets_.begin() + 1);

        const int total = comm.sum(count);
        activeCells_ = total;
        std::vector<int> all(comm.rank() == 0 ? static_cast<std::size_t>(total) : 0);
        comm.gatherv(cartesian.data(), count, all.data(), counts_.data(), offsets_.data(), 0);
        if (comm.rank() != 0) {
            return;
        }

        std::vector<std::size_t> order(all.size());
        std::iota(order.begin(), order.end(), std::size_t {0});
        std::sort(order.begin(), order.end(), [&all](std::size_t a, std::size_t b) {
            return all[a] < all[b];
        });
        activeOf_.assign(all.size(), 0);
        cartesianOf_.assign(all.size(), 0);
        for (std::size_t k = 0; k < order.size(); ++k) {
            activeOf_[order[k]] = k;
            cartesianOf_[k] = all[order[k]];
        }
    }

    // Collects one value per interior cell on rank 0, in active cell order.
    template <class Comm>
    std::vector<double> gather(const Comm& comm, const std::vector<double>& local)
    {
        const bool root = comm.rank() == 0;
        std::vector<double> received(root ? activeOf_.size() : 0);
        comm.gatherv(local.data(),
                     static_cast<int>(local.size()),
                     received.data(),
                     counts_.data(),
                     offsets_.data(),
                     0);
        std::vector<double> ordered(received.size());
        for (std::size_t k = 0; k < received.size(); ++k) {
            ordered[activeOf_[k]] = received[k];
        }
        return ordered;
    }

    void select(const CellSelection& selection, CellData& data) const
    {
        std::vector<std::size_t> picked;
        if (selection.mode == CellSelection::Mode::Indices) {
            for (const auto index : selection.indices) {
                if (index < 0 || index >= activeCells_) {
                    throw BadRequest("Active cell index " + std::to_string(index)
                                     + " is out of range");
                }
                picked.push_back(static_cast<std::size_t>(index));
            }
        } else if (selection.mode == CellSelection::Mode::Box) {
            const auto& box = selection.box;
            for (std::size_t d = 0; d < 3; ++d) {
                const int lo = box[2 * d];
                const int hi = box[2 * d + 1];
                if (lo < 1 || hi < lo || hi > dims_[d]) {
                    throw BadRequest("Cell box is outside the grid");
                }
            }
            const int nx = dims_[0];
            const int ny = dims_[1];
            for (std::size_t k = 0; k < cartesianOf_.size(); ++k) {
                const int c = cartesianOf_[k];
                const int i = c % nx + 1;
                const int j = (c / nx) % ny + 1;
                const int z = c / (nx * ny) + 1;
                if (i >= box[0] && i <= box[1] && j >= box[2] && j <= box[3] && z >= box[4]
                    && z <= box[5]) {
                    picked.push_back(k);
                    data.cells.push_back(static_cast<std::int64_t>(k));
                    data.ijk.insert(data.ijk.end(), {i, j, z});
                }
            }
        } else {
            return;
        }
        for (auto& field : data.fields) {
            std::vector<double> values;
            values.reserve(picked.size());
            for (const auto k : picked) {
                values.push_back(field.values[k]);
            }
            field.values = std::move(values);
        }
    }

    // Keeps the summary values after each report step on rank 0, so that a
    // client can ask for the history. flow first evaluates the summary in the
    // first report step.
    void recordSummary()
    {
        auto& simulator = *main_.getSimulatorPtr();
        if (simulator.vanguard().grid().comm().rank() != 0) {
            return;
        }
        const auto& state = simulator.vanguard().summaryState();
        std::vector<double> row(summaryKeys_.size(), std::numeric_limits<double>::quiet_NaN());
        for (const auto& [key, value] : state) {
            auto [it, added] = summaryColumn_.try_emplace(key, summaryKeys_.size());
            if (added) {
                summaryKeys_.push_back(key);
                row.push_back(value);
            } else {
                row[it->second] = value;
            }
        }
        summarySteps_.push_back(reportStep());
        summaryElapsed_.push_back(elapsedSeconds());
        summaryValues_.push_back(std::move(row));
    }

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
    bool stepped_ = false;
    StepReport lastStep_;

    std::array<int, 3> dims_ {};
    std::int64_t activeCells_ = 0;
    std::vector<unsigned> localCells_;
    std::vector<int> counts_;
    std::vector<int> offsets_;
    std::vector<std::size_t> activeOf_;
    std::vector<int> cartesianOf_;

    std::vector<std::string> summaryKeys_;
    std::unordered_map<std::string, std::size_t> summaryColumn_;
    std::vector<int> summarySteps_;
    std::vector<double> summaryElapsed_;
    std::vector<std::vector<double>> summaryValues_;
};

} // namespace Opm::Control

#endif // OPM_FLOW_CONTROL_DRIVER_HPP
