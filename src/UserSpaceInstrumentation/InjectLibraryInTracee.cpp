// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "InjectLibraryInTracee.h"

#include <absl/base/casts.h>
#include <absl/strings/match.h>
#include <absl/strings/str_cat.h>
#include <absl/strings/str_format.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include <csignal>
#include <regex>
#include <string>
#include <vector>

#include "AccessTraceesMemory.h"
#include "AllocateInTracee.h"
#include "ElfUtils/ElfFile.h"
#include "ElfUtils/LinuxMap.h"
#include "MachineCode.h"
#include "OrbitBase/Logging.h"
#include "UserSpaceInstrumentation/RegisterState.h"

namespace orbit_user_space_instrumentation {

namespace {

using orbit_elf_utils::ElfFile;
using orbit_elf_utils::ReadModules;

// Size of the small amount of memory we need in the tracee to write machine code into.
constexpr uint64_t kCodeScratchPadSize = 1024;

// In certain error conditions the tracee is damaged and we don't try to recover from that. We just
// abort with a fatal log message. None of these errors are expected to occur in operation
// obvioulsy. That's what the *OrDie methods below are for.
void FreeMemoryOrDie(pid_t pid, uint64_t address_code, uint64_t size) {
  auto result = FreeInTracee(pid, address_code, size);
  FAIL_IF(result.has_error(), "Unable to free previously allocated memory in tracee: \"%s\"",
          result.error().message());
}

void RestoreRegistersOrDie(RegisterState& register_state) {
  auto result = register_state.RestoreRegisters();
  FAIL_IF(result.has_error(), "Unable to restore register state in tracee: \"%s\"",
          result.error().message());
}

uint64_t GetReturnValueOrDie(pid_t pid) {
  RegisterState return_value_registers;
  auto result_return_value = return_value_registers.BackupRegisters(pid);
  FAIL_IF(result_return_value.has_error(), "Unable to read registers after function called :\"%s\"",
          result_return_value.error().message());
  return return_value_registers.GetGeneralPurposeRegisters()->x86_64.rax;
}

// Execute the code at `address_code`. The code segment has to end with an `int3`.
void ExecuteOrDie(pid_t pid, RegisterState& original_registers, uint64_t address_code) {
  RegisterState registers_set_rip = original_registers;
  registers_set_rip.GetGeneralPurposeRegisters()->x86_64.rip = address_code;
  auto result_restore_registers = registers_set_rip.RestoreRegisters();
  if (result_restore_registers.has_error()) {
    FATAL("Unable to set registers in tracee: \"%s\"", result_restore_registers.error().message());
  }
  if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) {
    FATAL("Unable to continue tracee with PTRACE_CONT.");
  }
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  if (waited != pid || !WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP) {
    FATAL("Failed to wait for sigtrap after PTRACE_CONT.");
  }
}

// Returns the absolute virtual address of a function in a module of a process as
// FindFunctionAddress does but accepts a fallback symbol if the primary one cannot be resolved.
ErrorMessageOr<uint64_t> FindFunctionAddressWithFallback(pid_t pid, std::string_view function,
                                                         std::string_view module,
                                                         std::string_view fallback_function,
                                                         std::string_view fallback_module) {
  ErrorMessageOr<uint64_t> primary_address_or_error = FindFunctionAddress(pid, function, module);
  if (primary_address_or_error.has_value()) {
    return primary_address_or_error.value();
  }
  ErrorMessageOr<uint64_t> fallback_address_or_error =
      FindFunctionAddress(pid, fallback_function, fallback_module);
  if (fallback_address_or_error.has_value()) {
    return fallback_address_or_error.value();
  }

  return ErrorMessage(absl::StrFormat(
      "Failed to load symbol \"%s\" from module \"%s\" with error: \"%s\"\nAnd also "
      "failed to load fallback symbol \"%s\" from module \"%s\" with error: \"%s\"",
      function, module, primary_address_or_error.error().message(), fallback_function,
      fallback_module, fallback_address_or_error.error().message()));
}

}  // namespace

[[nodiscard]] ErrorMessageOr<void*> DlopenInTracee(pid_t pid, std::filesystem::path path,
                                                   uint32_t flag) {
  // Figure out address of dlopen in libc.
  OUTCOME_TRY(address_dlopen, FindFunctionAddressWithFallback(pid, "dlopen", "libdl",
                                                              "__libc_dlopen_mode", "libc"));

  // Backup registers.
  RegisterState original_registers;
  OUTCOME_TRY(original_registers.BackupRegisters(pid));

  // Allocate small memory area in the tracee. This is used for the code and the path name.
  const uint64_t path_length = path.string().length() + 1;  // Include terminating zero.
  const uint64_t memory_size = kCodeScratchPadSize + path_length;
  OUTCOME_TRY(address_code, AllocateInTracee(pid, memory_size));

  // Write the name of the .so into memory at address_code with offset of kCodeScratchPadSize.
  std::vector<uint8_t> path_as_vector(path_length);
  memcpy(path_as_vector.data(), path.c_str(), path_length);
  const uint64_t address_so_path = address_code + kCodeScratchPadSize;
  auto result_write_path = WriteTraceesMemory(pid, address_so_path, path_as_vector);
  if (result_write_path.has_error()) {
    FreeMemoryOrDie(pid, address_code, memory_size);
    return result_write_path.error();
  }

  // We want to do the following in the tracee:
  // return_value = dlopen(path, flag);
  // The calling convention is to put the parameters in registers rdi and rsi.
  // So the address of the file path goes to rdi. The flag argument goes into rsi. Then we load the
  // address of dlopen into rax and do the call. Assembly in Intel syntax (destination first),
  // machine code on the right:

  // movabsq rdi, address_so_path     48 bf address_so_path
  // movl esi, flag                   be flag
  // movabsq rax, address_dlopen      48 b8 address_dlopen
  // call rax                         ff d0
  // int3                             cc
  MachineCode code;
  code.AppendBytes({0x48, 0xbf})
      .AppendImmediate64(address_so_path)
      .AppendBytes({0xbe})
      .AppendImmediate32(flag)
      .AppendBytes({0x48, 0xb8})
      .AppendImmediate64(address_dlopen)
      .AppendBytes({0xff, 0xd0})
      .AppendBytes({0xcc});
  auto result_write_code = WriteTraceesMemory(pid, address_code, code.GetResultAsVector());
  if (result_write_code.has_error()) {
    FreeMemoryOrDie(pid, address_code, memory_size);
    return result_write_code.error();
  }

  ExecuteOrDie(pid, original_registers, address_code);

  void* return_value = absl::bit_cast<void*>(GetReturnValueOrDie(pid));

  // Clean up memory and registers.
  RestoreRegistersOrDie(original_registers);
  FreeMemoryOrDie(pid, address_code, memory_size);
  return return_value;
}

[[nodiscard]] ErrorMessageOr<void*> DlsymInTracee(pid_t pid, void* handle,
                                                  std::string_view symbol) {
  // Figure out address of dlsym in libc.
  OUTCOME_TRY(address_dlsym,
              FindFunctionAddressWithFallback(pid, "dlsym", "libdl", "__libc_dlsym", "libc"));

  // Backup registers.
  RegisterState original_registers;
  OUTCOME_TRY(original_registers.BackupRegisters(pid));

  // Allocate small memory area in the tracee. This is used for the code and the symbol name.
  const size_t symbol_name_length = symbol.length() + 1;  // include terminating zero
  const uint64_t memory_size = kCodeScratchPadSize + symbol_name_length;
  OUTCOME_TRY(address_code, AllocateInTracee(pid, memory_size));

  // Write the name of symbol into memory at address_code with offset of kCodeScratchPadSize.
  std::vector<uint8_t> symbol_name_as_vector(symbol_name_length, 0);
  memcpy(symbol_name_as_vector.data(), symbol.data(), symbol.length());
  const uint64_t address_symbol_name = address_code + kCodeScratchPadSize;
  auto result_write_symbol_name =
      WriteTraceesMemory(pid, address_symbol_name, symbol_name_as_vector);
  if (result_write_symbol_name.has_error()) {
    FreeMemoryOrDie(pid, address_code, memory_size);
    return result_write_symbol_name.error();
  }

  // We want to do the following in the tracee:
  // return_value = dlsym(handle, symbol);
  // The calling convention is to put the parameters in registers rdi and rsi.
  // So the handle goes to rdi and the address of the symbol name goes to rsi. Then we load the
  // address of dlsym into rax and do the call. Assembly in Intel syntax (destination first),
  // machine code on the right:

  // movabsq rdi, handle              48 bf handle
  // movabsq rsi, address_symbol_name 48 be address_symbol_name
  // movabsq rax, address_dlsym       48 b8 address_dlsym
  // call rax                         ff d0
  // int3                             cc
  MachineCode code;
  code.AppendBytes({0x48, 0xbf})
      .AppendImmediate64(absl::bit_cast<uint64_t>(handle))
      .AppendBytes({0x48, 0xbe})
      .AppendImmediate64(address_symbol_name)
      .AppendBytes({0x48, 0xb8})
      .AppendImmediate64(address_dlsym)
      .AppendBytes({0xff, 0xd0})
      .AppendBytes({0xcc});
  auto result_write_code = WriteTraceesMemory(pid, address_code, code.GetResultAsVector());
  if (result_write_code.has_error()) {
    FreeMemoryOrDie(pid, address_code, memory_size);
    return result_write_code.error();
  }

  ExecuteOrDie(pid, original_registers, address_code);

  void* return_value = absl::bit_cast<void*>(GetReturnValueOrDie(pid));

  // Cleanup memory and registers.
  RestoreRegistersOrDie(original_registers);
  FreeMemoryOrDie(pid, address_code, memory_size);
  return return_value;
}

[[nodiscard]] ErrorMessageOr<void> DlcloseInTracee(pid_t pid, void* handle) {
  // Figure out address of dlclose.
  OUTCOME_TRY(address_dlclose,
              FindFunctionAddressWithFallback(pid, "dlclose", "libdl", "__libc_dlclose", "libc"));

  // Backup registers.
  RegisterState original_registers;
  OUTCOME_TRY(original_registers.BackupRegisters(pid));

  // Allocate small memory area in the tracee.
  OUTCOME_TRY(address_code, AllocateInTracee(pid, kCodeScratchPadSize));

  // We want to do the following in the tracee:
  // dlclose(handle);
  // The calling convention is to put the parameter in registers rdi. Then we load the address of
  // address_dlclose into rax and do the call. Assembly in Intel syntax (destination first), machine
  // code on the right:

  // movabsq rdi, handle              48 bf handle
  // movabsq rax, address_dlclose     48 b8 address_dlclose
  // call rax                         ff d0
  // int3                             cc
  MachineCode code;
  code.AppendBytes({0x48, 0xbf})
      .AppendImmediate64(absl::bit_cast<uint64_t>(handle))
      .AppendBytes({0x48, 0xb8})
      .AppendImmediate64(address_dlclose)
      .AppendBytes({0xff, 0xd0})
      .AppendBytes({0xcc});
  auto result_write_code = WriteTraceesMemory(pid, address_code, code.GetResultAsVector());
  if (result_write_code.has_error()) {
    FreeMemoryOrDie(pid, address_code, kCodeScratchPadSize);
    return result_write_code.error();
  }

  ExecuteOrDie(pid, original_registers, address_code);

  if (GetReturnValueOrDie(pid) != 0) {
    FATAL("Unable to unload dynamic library from tracee.");
  }

  // Cleanup memory and registers.
  RestoreRegistersOrDie(original_registers);
  FreeMemoryOrDie(pid, address_code, kCodeScratchPadSize);
  return outcome::success();
}

ErrorMessageOr<uint64_t> FindFunctionAddress(pid_t pid, std::string_view function_name,
                                             std::string_view module_prefix) {
  auto modules = ReadModules(pid);
  if (modules.has_error()) {
    return modules.error();
  }

  std::string module_file_path;
  uint64_t module_base_address = 0;
  for (const auto& m : modules.value()) {
    // This matches the name of the module followed by any (possibly empty) combination of `.`, `-`
    // and digits and a single occurance of the letters `so`.
    // If module is `libc` this matches `libc-2.31.so`, `libc`, `libc1.so` and also `libcso-9-2...-`
    // but not `libc-something-3.14.so` or `i-am-not-libc-2.31.so`
    std::string re_as_string = absl::StrCat(module_prefix, "[\\.\\-0-9]*(so)*[\\.\\-0-9]*");
    if (std::regex_match(m.name(), std::regex(re_as_string))) {
      module_file_path = m.file_path();
      module_base_address = m.address_start();
    }
  }
  if (module_file_path.empty()) {
    return ErrorMessage(
        absl::StrFormat("There is no module \"%s\" in process %d.", module_prefix, pid));
  }

  auto elf_file = ElfFile::Create(module_file_path);
  if (elf_file.has_error()) {
    return elf_file.error();
  }

  auto syms = elf_file.value()->LoadSymbolsFromDynsym();
  if (syms.has_error()) {
    return ErrorMessage(absl::StrFormat("Failed to load symbols for module \"%s\": %s",
                                        module_prefix, syms.error().message()));
  }

  for (const auto& sym : syms.value().symbol_infos()) {
    if (sym.name() == function_name) {
      return sym.address() + module_base_address - syms.value().load_bias();
    }
  }

  return ErrorMessage(absl::StrFormat("Unable to locate function symbol \"%s\" in module \"%s\".",
                                      function_name, module_prefix));
}

}  // namespace orbit_user_space_instrumentation