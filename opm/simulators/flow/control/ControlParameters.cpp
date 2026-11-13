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

#include <opm/simulators/flow/control/ControlParameters.hpp>

#include <opm/models/utils/parametersystem.hpp>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace Opm::Control
{

namespace
{

    std::string readFile(const std::string& path, const std::string& what)
    {
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("Cannot read " + what + " " + path);
        }
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    std::string readToken(const std::string& path)
    {
        std::ifstream in(path);
        if (!in) {
            throw std::runtime_error("Cannot read control token file " + path);
        }
        std::string token {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        const auto end = token.find_last_not_of(" \t\r\n");
        token.erase(end == std::string::npos ? 0 : end + 1);
        if (token.empty()) {
            throw std::runtime_error("Control token file " + path + " is empty");
        }
        return token;
    }

} // namespace

void
registerControlParameters()
{
    Parameters::Register<Parameters::ControlEndpoint>(
        "Address to listen on for step control, for example 127.0.0.1:50051. "
        "Empty runs the schedule to the end without control.");
    Parameters::Register<Parameters::ControlTokenFile>(
        "File holding the bearer token clients must send. "
        "Required unless the endpoint is a loopback address.");
    Parameters::Register<Parameters::ControlIdleTimeout>(
        "Seconds to wait for a command before acting on --control-on-disconnect.");
    Parameters::Register<Parameters::ControlOnDisconnect>(
        "What to do when no command arrives within the idle timeout: pause writes the "
        "output so far and exits, finish runs the rest of the schedule, abort exits "
        "with an error.");
    Parameters::Register<Parameters::ControlTlsCert>(
        "PEM certificate for TLS on the control endpoint.");
    Parameters::Register<Parameters::ControlTlsKey>(
        "PEM private key for TLS on the control endpoint.");
}

Options
controlOptions([[maybe_unused]] bool root)
{
    Options options;
    options.endpoint = Parameters::Get<Parameters::ControlEndpoint>();
    if (options.endpoint.empty()) {
        return options;
    }
#if !HAVE_CONTROL_SERVER
    throw std::runtime_error("--control-endpoint is set but this build has no control server");
#else
    const std::string tokenFile = Parameters::Get<Parameters::ControlTokenFile>();
    if (root && !tokenFile.empty()) {
        options.token = readToken(tokenFile);
    }
    const int timeout = Parameters::Get<Parameters::ControlIdleTimeout>();
    if (timeout <= 0) {
        throw std::invalid_argument("--control-idle-timeout must be positive");
    }
    options.idleTimeout = std::chrono::seconds(timeout);

    const std::string onDisconnect = Parameters::Get<Parameters::ControlOnDisconnect>();
    if (onDisconnect == "pause") {
        options.onDisconnect = OnDisconnect::Pause;
    } else if (onDisconnect == "finish") {
        options.onDisconnect = OnDisconnect::Finish;
    } else if (onDisconnect == "abort") {
        options.onDisconnect = OnDisconnect::Abort;
    } else {
        throw std::invalid_argument("--control-on-disconnect must be pause, finish or abort");
    }

    const std::string cert = Parameters::Get<Parameters::ControlTlsCert>();
    const std::string key = Parameters::Get<Parameters::ControlTlsKey>();
    if (cert.empty() != key.empty()) {
        throw std::invalid_argument("--control-tls-cert and --control-tls-key go together");
    }
    if (root && !cert.empty()) {
        options.tlsCertificate = readFile(cert, "control TLS certificate");
        options.tlsKey = readFile(key, "control TLS key");
    }
    return options;
#endif
}

} // namespace Opm::Control
