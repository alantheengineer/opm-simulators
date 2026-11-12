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

#include <config.h>

#include <opm/simulators/flow/control/ControlServer.hpp>

#include <stdexcept>

// Used when the build has no gRPC. controlOptions() rejects an endpoint
// before this is reached.

namespace Opm::Control
{

class ControlServer::Impl
{
};

ControlServer::ControlServer(const Options&)
{
    throw std::runtime_error("This build has no control server");
}

ControlServer::~ControlServer() = default;

int
ControlServer::port() const
{
    return 0;
}

std::optional<Command>
ControlServer::waitForCommand()
{
    return std::nullopt;
}

void
ControlServer::reply(Reply)
{
}

void
ControlServer::publishStatus(const Status&)
{
}

} // namespace Opm::Control
