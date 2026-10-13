// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Core::CrashReport {

/// Installs the last-chance handlers (unhandled exceptions and std::terminate) that write a
/// crash report. Call once at startup.
void Install();

/// Writes a crash report to the log folder:
///  - crash_<date>_<time>.txt: what failed, where (host and guest module + offset), registers
///    and a stack trace,
///  - crash_<date>_<time>.dmp: a minidump for debuggers,
///  - crash_<date>_<time>_log.txt: a copy of the emulator log at the time of the crash, so that
///    starting the emulator again does not overwrite it.
/// Only the first call does anything. exception_pointers is an EXCEPTION_POINTERS* on Windows,
/// or null to capture the calling thread's current state.
void Write(std::string_view reason, void* exception_pointers = nullptr) noexcept;

/// Command line switch for the helper process that writes the minidump of a crashed emulator.
constexpr std::string_view DumpWriterSwitch = "--write-crash-dump";

/// Entry point of that helper process: writes the minidump and returns the exit code.
int RunDumpWriter();

} // namespace Core::CrashReport
