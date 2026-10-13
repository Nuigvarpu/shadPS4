// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>
#include <fmt/format.h>

#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include "common/singleton.h"
#include "core/crash_report.h"
#include "core/linker.h"
#include "core/module.h"

#ifdef _WIN32
#include <windows.h>
// windows.h must come first.
#include <dbghelp.h>
#include <intrin.h>
#include <shellapi.h>
#endif

namespace Core::CrashReport {

namespace {

std::atomic_flag g_written = ATOMIC_FLAG_INIT;

#ifdef _WIN32

/// Reads guest or host memory without faulting if it is not readable.
bool SafeRead(u64 address, u64& value) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &value,
                             sizeof(value), &read) &&
           read == sizeof(value);
}

/// Name and offset of whatever contains the address: a host module (shadPS4.exe or a DLL) or a
/// guest module of the game. Empty if it is not inside any known module.
std::string DescribeAddress(u64 address) {
    if (address == 0) {
        return {};
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module != nullptr) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        const std::string name = std::filesystem::path{path}.filename().string();
        return fmt::format("{}+{:#x}", name, address - reinterpret_cast<u64>(module));
    }
    if (Common::ElfInfo::Instance().IsInitialized()) {
        auto* linker = Common::Singleton<Core::Linker>::Instance();
        if (const Core::Module* guest = linker->FindByAddress(address)) {
            return fmt::format("[juego] {}+{:#x}", guest->name, address - guest->GetBaseAddress());
        }
    }
    return {};
}

/// One unwind step for host code that has unwind information. Kept free of C++ objects so it
/// can use SEH: a corrupted stack must not crash the crash handler.
bool UnwindHostFrame(CONTEXT* context) {
    __try {
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context->Rip, &image_base, nullptr);
        if (function == nullptr) {
            return false;
        }
        PVOID handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context->Rip, function, context,
                         &handler_data, &establisher_frame, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string StackTrace(const CONTEXT& start) {
    std::string out;
    CONTEXT context = start;
    for (int frame = 0; frame < 48 && context.Rip != 0; ++frame) {
        out +=
            fmt::format("  #{:02} {:#018x} {}\n", frame, context.Rip, DescribeAddress(context.Rip));
        const u64 previous_rsp = context.Rsp;
        if (UnwindHostFrame(&context)) {
            continue;
        }
        // No unwind data (guest code or a leaf function): follow the frame pointer chain if it
        // looks sane, otherwise assume a leaf function with the return address on top.
        u64 saved_rbp = 0;
        u64 return_address = 0;
        if (context.Rbp > context.Rsp && context.Rbp - context.Rsp < 1_MB &&
            SafeRead(context.Rbp, saved_rbp) && SafeRead(context.Rbp + 8, return_address)) {
            context.Rsp = context.Rbp + 16;
            context.Rbp = saved_rbp;
            context.Rip = return_address;
        } else if (SafeRead(context.Rsp, return_address)) {
            context.Rsp += 8;
            context.Rip = return_address;
        } else {
            break;
        }
        if (context.Rsp <= previous_rsp) {
            break;
        }
    }
    return out;
}

/// Values on the stack that point into a known module. Not every entry is a real caller, but
/// this works even when the unwinder above gets lost in guest code.
std::string StackScan(u64 rsp) {
    std::string out;
    int found = 0;
    for (u64 i = 0; i < 1024 && found < 40; ++i) {
        u64 value = 0;
        if (!SafeRead(rsp + i * 8, value)) {
            break;
        }
        if (value < 0x10000) {
            continue;
        }
        const std::string where = DescribeAddress(value);
        if (!where.empty()) {
            out += fmt::format("  [rsp+{:#06x}] {:#018x} {}\n", i * 8, value, where);
            ++found;
        }
    }
    return out;
}

std::string ExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "EXCEPTION_ACCESS_VIOLATION (acceso a memoria invalida)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:
        return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_IN_PAGE_ERROR:
        return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_BREAKPOINT:
        return "EXCEPTION_BREAKPOINT";
    case 0xE06D7363:
        return "C++ exception";
    default:
        return "unknown";
    }
}

std::string CpuName() {
    int regs[4]{};
    char brand[49]{};
    __cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) < 0x80000004) {
        return "unknown";
    }
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        std::memcpy(brand + i * 16, regs, sizeof(regs));
    }
    std::string name{brand};
    name.erase(0, name.find_first_not_of(' '));
    return name;
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
    if (std::FILE* file = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}

constexpr auto MinidumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules);

/// Writes the minidump from a helper process (this same executable, started with
/// --write-crash-dump). Writing a dump of the own process can deadlock: it suspends every other
/// thread, and one of them may hold a lock (e.g. the heap) that the dump writer needs.
void WriteMinidump(const std::filesystem::path& path, EXCEPTION_POINTERS* exception,
                   DWORD thread_id) {
    std::wstring exe(32768, L'\0');
    const DWORD exe_length =
        GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (exe_length == 0 || exe_length >= exe.size()) {
        return;
    }
    exe.resize(exe_length);
    std::wstring command =
        L"\"" + exe + L"\" --write-crash-dump " + std::to_wstring(GetCurrentProcessId()) + L" " +
        std::to_wstring(thread_id) + L" " + std::to_wstring(reinterpret_cast<u64>(exception)) +
        L" \"" + path.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return;
    }
    if (WaitForSingleObject(process.hProcess, 30'000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

void WriteReport(std::string_view reason, EXCEPTION_POINTERS* exception, DWORD thread_id) {
    const CONTEXT& ctx = *exception->ContextRecord;
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    const std::string stamp =
        fmt::format("crash_{:04}{:02}{:02}_{:02}{:02}{:02}", now.wYear, now.wMonth, now.wDay,
                    now.wHour, now.wMinute, now.wSecond);
    const auto log_dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);

    std::string text;
    text += "shadPS4 - informe de fallo (crash report)\n";
    text += "==========================================\n";
    text +=
        fmt::format("Envia estos 3 archivos ({0}.txt, {0}.dmp y {0}_log.txt) junto con\n", stamp);
    text += "una descripcion de lo que estabas haciendo en el juego cuando se cerro.\n";
    text += "Si el juego siguio funcionando con normalidad despues de esta hora, puedes ignorar\n";
    text += "este informe.\n\n";
    text += fmt::format("Fecha: {:04}-{:02}-{:02} {:02}:{:02}:{:02}\n", now.wYear, now.wMonth,
                        now.wDay, now.wHour, now.wMinute, now.wSecond);
    text += fmt::format("Emulador: {} {} ({})\n", Common::g_version, Common::g_scm_branch,
                        Common::g_scm_desc);
    text += fmt::format("CPU: {}\n", CpuName());
    const auto& game = Common::ElfInfo::Instance();
    if (game.IsInitialized()) {
        text += fmt::format("Juego: {} {} version {}\n", game.GameSerial(), game.Title(),
                            game.AppVer());
    }
    text += fmt::format("Hilo: {}\n\n", thread_id);

    text += fmt::format("Motivo: {}\n", reason);
    text += fmt::format("Excepcion: {:#x} {}\n", record.ExceptionCode,
                        ExceptionName(record.ExceptionCode));
    const u64 fault_address = reinterpret_cast<u64>(record.ExceptionAddress);
    text += fmt::format("Direccion: {:#018x} {}\n", fault_address, DescribeAddress(fault_address));
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        text += fmt::format("Acceso: {} en {:#018x}\n",
                            kind == 0   ? "lectura"
                            : kind == 1 ? "escritura"
                                        : "ejecucion",
                            record.ExceptionInformation[1]);
    }
    text += "\nRegistros:\n";
    text += fmt::format("  rax={:#018x} rbx={:#018x} rcx={:#018x} rdx={:#018x}\n", ctx.Rax, ctx.Rbx,
                        ctx.Rcx, ctx.Rdx);
    text += fmt::format("  rsi={:#018x} rdi={:#018x} rbp={:#018x} rsp={:#018x}\n", ctx.Rsi, ctx.Rdi,
                        ctx.Rbp, ctx.Rsp);
    text += fmt::format("  r8 ={:#018x} r9 ={:#018x} r10={:#018x} r11={:#018x}\n", ctx.R8, ctx.R9,
                        ctx.R10, ctx.R11);
    text += fmt::format("  r12={:#018x} r13={:#018x} r14={:#018x} r15={:#018x}\n", ctx.R12, ctx.R13,
                        ctx.R14, ctx.R15);
    text += fmt::format("  rip={:#018x} eflags={:#x}\n", ctx.Rip, ctx.EFlags);
    text += "\nPila (stack trace):\n";
    text += StackTrace(ctx);
    text += "\nDirecciones conocidas en la pila (stack scan):\n";
    text += StackScan(ctx.Rsp);
    WriteText(log_dir / (stamp + ".txt"), text);

    // A copy of the log: starting the emulator again overwrites the original. Give the
    // asynchronous logger a moment to write out the last messages first.
    Common::Log::Flush();
    Sleep(300);
    Common::Log::Flush();
    const std::filesystem::path log_file = Common::Log::CurrentLogFile();
    if (!log_file.empty()) {
        std::filesystem::copy_file(log_file, log_dir / (stamp + "_log.txt"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
    }

    // The minidump last: it takes longest, and the text report and log are already safe.
    WriteMinidump(log_dir / (stamp + ".dmp"), exception, thread_id);
}

struct ReportJob {
    std::string_view reason;
    EXCEPTION_POINTERS* exception;
    DWORD thread_id;
};

DWORD WINAPI ReportThread(LPVOID param) {
    const auto* job = static_cast<const ReportJob*>(param);
    try {
        WriteReport(job->reason, job->exception, job->thread_id);
    } catch (...) {
    }
    return 0;
}

/// Writes the report from a separate thread with a fresh stack: the crashing thread may be out
/// of stack (or on a small guest stack), and a minidump is more reliable when written from
/// another thread.
void WriteReportOnHelperThread(std::string_view reason, EXCEPTION_POINTERS* exception) {
    // Without exception information (assert, std::terminate), capture this thread's state.
    CONTEXT captured_context{};
    EXCEPTION_RECORD captured_record{};
    EXCEPTION_POINTERS captured{};
    if (exception == nullptr || exception->ContextRecord == nullptr ||
        exception->ExceptionRecord == nullptr) {
        RtlCaptureContext(&captured_context);
        captured_record.ExceptionCode = EXCEPTION_BREAKPOINT;
        captured_record.ExceptionAddress = reinterpret_cast<PVOID>(captured_context.Rip);
        captured.ContextRecord = &captured_context;
        captured.ExceptionRecord = &captured_record;
        exception = &captured;
    }
    ReportJob job{reason, exception, GetCurrentThreadId()};
    const HANDLE thread = CreateThread(nullptr, 1_MB, ReportThread, &job, 0, nullptr);
    if (thread == nullptr) {
        ReportThread(&job);
        return;
    }
    WaitForSingleObject(thread, 45'000);
    CloseHandle(thread);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* exception) {
    Write("Excepcion no controlada", exception);
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif

[[noreturn]] void OnTerminate() {
    std::string reason = "std::terminate";
    if (const auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& e) {
            reason = fmt::format("Excepcion de C++ no capturada: {}", e.what());
        } catch (...) {
            reason = "Excepcion de C++ no capturada";
        }
    }
    Write(reason);
    std::abort();
}

} // Anonymous namespace

int RunDumpWriter() {
#ifdef _WIN32
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr || argc != 6) {
        return 1;
    }
    const auto pid = static_cast<DWORD>(std::wcstoul(argv[2], nullptr, 10));
    const auto tid = static_cast<DWORD>(std::wcstoul(argv[3], nullptr, 10));
    const auto exception_address = std::wcstoull(argv[4], nullptr, 10);
    const std::wstring path = argv[5];
    LocalFree(argv);

    const HANDLE process = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (process == nullptr) {
        return 1;
    }
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        CloseHandle(process);
        return 1;
    }
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ThreadId = tid;
    info.ExceptionPointers = reinterpret_cast<PEXCEPTION_POINTERS>(exception_address);
    info.ClientPointers = TRUE; // The pointers are in the crashed process.
    const BOOL ok = MiniDumpWriteDump(process, pid, file, MinidumpType,
                                      exception_address != 0 ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    CloseHandle(process);
    return ok ? 0 : 1;
#else
    return 1;
#endif
}

void Install() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(UnhandledFilter);
#endif
    std::set_terminate(OnTerminate);
}

void Write(std::string_view reason, [[maybe_unused]] void* exception_pointers) noexcept {
    if (g_written.test_and_set()) {
        return;
    }
#ifdef _WIN32
    WriteReportOnHelperThread(reason, static_cast<EXCEPTION_POINTERS*>(exception_pointers));
#endif
}

} // namespace Core::CrashReport
