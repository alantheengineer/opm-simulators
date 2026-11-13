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

#ifndef OPM_CONTROL_WATCHDOG_HPP
#define OPM_CONTROL_WATCHDOG_HPP

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace Opm::Control
{

// Runs an action on its own thread unless destroyed within the timeout.
// The action must not touch MPI or the simulator.
class Watchdog
{
public:
    Watchdog(std::chrono::milliseconds timeout, std::function<void()> action)
        : thread_([this, timeout, action = std::move(action)] {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, timeout, [this] { return stopped_; })) {
                action();
            }
        })
    {
    }

    ~Watchdog()
    {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        cv_.notify_one();
        thread_.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    Watchdog(Watchdog&&) = delete;
    Watchdog& operator=(Watchdog&&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
    std::thread thread_;
};

} // namespace Opm::Control

#endif // OPM_CONTROL_WATCHDOG_HPP
