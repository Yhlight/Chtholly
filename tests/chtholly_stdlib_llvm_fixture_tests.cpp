#include "chtholly/Compiler/CompilationUnit.h"
#include "test_target.h"
#include "chtholly/Compiler/InteropArtifact.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using chtholly::compiler::CompilationSession;
using chtholly::compiler::CompilationUnitKind;
using chtholly::compiler::SourceInput;
using chtholly::compiler::interop::ArtifactBundle;

namespace {
// The fixture verifies the source-level std::net API, Result wrapper, and CFDL closure.
void checkAt(bool condition, const char *expression, int line) {
  if (!condition) {
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::abort();
  }
}
#define check(condition) checkAt((condition), #condition, __LINE__)
} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "chtholly-next-stdlib-llvm-fixture";
  const auto sidecar = root / "std-host.interop";
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  std::filesystem::create_directories(root);

  std::ifstream input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                      "stdlib" / "host.cfdl");
  std::ifstream net_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                          "stdlib" / "net.cfdl");
  std::ifstream sync_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                           "stdlib" / "sync.cfdl");
  std::ifstream sync_api_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                               "stdlib" / "sync.cns");
  std::ifstream channel_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                              "stdlib" / "channel.cfdl");
  std::ifstream net_api_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                              "stdlib" / "net.cns");
  std::ifstream error_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                            "stdlib" / "error.cns");
  std::ifstream ops_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                          "stdlib" / "ops.cns");
  std::ifstream callable_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                               "stdlib" / "callable.cns");
  std::ifstream result_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                             "stdlib" / "result.cns");
  std::ifstream compare_input(std::filesystem::path(CHTHOLLY_SOURCE_DIR) /
                              "stdlib" / "compare.cns");
  std::ostringstream source;
  source << input.rdbuf();
  source << "\nforeign fn fixture_probe() -> i32 "
            "link \"chtholly_next_host_v1_fixture_probe\";\n";
  check(input.good() || input.eof());
  std::ostringstream net_source;
  net_source << net_input.rdbuf();
  std::ostringstream sync_source;
  sync_source << sync_input.rdbuf();
  std::ostringstream sync_api_source;
  sync_api_source << sync_api_input.rdbuf();
  const auto sync_api_text = sync_api_source.str();
  const auto condvar_close = sync_api_text.find("pub fn condvar_close");
  check(condvar_close != std::string::npos);
  check(sync_api_text.find("std::error::from_sync(status)", condvar_close) !=
        std::string::npos);
  std::ostringstream channel_source;
  channel_source << channel_input.rdbuf();
  std::ostringstream net_api_source;
  net_api_source << net_api_input.rdbuf();
  std::ostringstream error_source;
  error_source << error_input.rdbuf();
  std::ostringstream ops_source;
  ops_source << ops_input.rdbuf();
  std::ostringstream callable_source;
  callable_source << callable_input.rdbuf();
  std::ostringstream result_source;
  result_source << result_input.rdbuf();
  std::ostringstream compare_source;
  compare_source << compare_input.rdbuf();
  check(net_input.good() || net_input.eof());
  check(sync_input.good() || sync_input.eof());
  check(sync_api_input.good() || sync_api_input.eof());
  check(channel_input.good() || channel_input.eof());
  check(net_api_input.good() || net_api_input.eof());
  check(error_input.good() || error_input.eof());
  check(ops_input.good() || ops_input.eof());
  check(callable_input.good() || callable_input.eof());
  check(result_input.good() || result_input.eof());
  check(compare_input.good() || compare_input.eof());

  CompilationSession session(chtholly_test::targetTriple, "std-host-fixture");
  const auto binding = session.addUnit(
      SourceInput("host.cfdl", source.str()),
      CompilationUnitKind::ForeignBinding);
  check(binding.hasValue());
  const auto net_binding = session.addUnit(
      SourceInput("net.cfdl", net_source.str()),
      CompilationUnitKind::ForeignBinding);
  check(net_binding.hasValue());
  const auto sync_binding = session.addUnit(
      SourceInput("sync.cfdl", sync_source.str()),
      CompilationUnitKind::ForeignBinding);
  check(sync_binding.hasValue());
  const auto sync_api = session.addUnit(
      SourceInput("sync.cns", sync_api_source.str()),
      CompilationUnitKind::ChthollySource);
  check(sync_api.hasValue());
  const auto channel_binding = session.addUnit(
      SourceInput("channel.cfdl", channel_source.str()),
      CompilationUnitKind::ForeignBinding);
  check(channel_binding.hasValue());
  const auto net_api = session.addUnit(
      SourceInput("net.cns", net_api_source.str()),
      CompilationUnitKind::ChthollySource);
  check(net_api.hasValue());
  check(session.addUnit(SourceInput("error.cns", error_source.str()),
                        CompilationUnitKind::ChthollySource)
            .hasValue());
  check(session.addUnit(SourceInput("ops.cns", ops_source.str()),
                        CompilationUnitKind::ChthollySource)
            .hasValue());
  check(session.addUnit(SourceInput("callable.cns", callable_source.str()),
                        CompilationUnitKind::ChthollySource)
            .hasValue());
  check(session.addUnit(SourceInput("result.cns", result_source.str()),
                        CompilationUnitKind::ChthollySource)
            .hasValue());
  check(session.addUnit(SourceInput("compare.cns", compare_source.str()),
                        CompilationUnitKind::ChthollySource)
            .hasValue());
  const auto consumer = session.addUnit(SourceInput(
      "main.cns",
      "module main; import std::host; import std::net; import std::net::raw; import std::sync; import std::sync::raw; import std::channel; fn main(): i32 { unsafe { "
      "let handle: std::host::Handle; "
      "let opened = std::host::host_open(c\"fixture.tmp\", 11u64, handle); "
      "if (opened != 0) { return opened; } "
            "std::host::host_close(move handle); "
            "let listener: std::net::raw::Socket; "
            "let net_status = std::net::raw::listen(39232u16, listener); "
            "if (net_status != 0) { return net_status; } "
            "std::net::raw::close(move listener); "
            "var mutex: std::sync::raw::Mutex; "
            "let mutex_status = std::sync::raw::mutex_init(mutex); "
            "if (mutex_status != 0) { return mutex_status; } "
            "std::sync::raw::mutex_lock(&mutex); std::sync::raw::mutex_unlock(&mutex); "
            "std::sync::raw::mutex_close(move mutex); "
            "var channel: std::channel::Channel; "
            "let channel_status = std::channel::channel_init(8u64, channel); "
            "if (channel_status != 0) { return channel_status; } "
            "std::channel::channel_close(move channel); "
            "return std::host::fixture_probe(); } }"));
  check(consumer.hasValue());
  const std::vector<std::pair<std::string, std::string>> runtime_symbols = {
      {"host_open", "chtholly_next_host_v1_open"},
      {"host_read", "chtholly_next_host_v1_read_ref"},
      {"host_write", "chtholly_next_host_v1_write_ref"},
      {"host_close", "chtholly_next_host_v1_close"},
      {"monotonic_now", "chtholly_next_host_v1_monotonic_now"},
      {"task_spawn", "chtholly_next_host_v1_task_spawn"},
      {"task_poll", "chtholly_next_host_v1_task_poll_ref"},
      {"task_cancel", "chtholly_next_host_v1_task_cancel_ref"},
      {"task_wake", "chtholly_next_host_v1_task_wake_ref"},
      {"task_join", "chtholly_next_host_v1_task_join"},
      {"accept", "chtholly_next_host_v1_net_accept_ref"},
      {"close", "chtholly_next_host_v1_net_close"},
      {"listen", "chtholly_next_host_v1_net_listen"},
      {"read", "chtholly_next_host_v1_net_read_ref"},
      {"write", "chtholly_next_host_v1_net_write_ref"},
      {"mutex_close", "chtholly_next_host_v1_sync_mutex_close"},
      {"mutex_init", "chtholly_next_host_v1_sync_mutex_init"},
      {"mutex_lock", "chtholly_next_host_v1_sync_mutex_lock_ref"},
      {"mutex_unlock", "chtholly_next_host_v1_sync_mutex_unlock_ref"},
      {"condvar_close", "chtholly_next_host_v1_sync_condvar_close"},
      {"condvar_init", "chtholly_next_host_v1_sync_condvar_init"},
      {"condvar_notify_all", "chtholly_next_host_v1_sync_condvar_notify_all_ref"},
      {"condvar_notify_one", "chtholly_next_host_v1_sync_condvar_notify_one_ref"},
      {"condvar_wait", "chtholly_next_host_v1_sync_condvar_wait_ref"},
      {"channel_close", "chtholly_next_host_v1_channel_close"},
      {"channel_init", "chtholly_next_host_v1_channel_init"},
      {"channel_receive", "chtholly_next_host_v1_channel_receive_ref"},
      {"channel_send", "chtholly_next_host_v1_channel_send_ref"},
      {"fixture_probe", "chtholly_next_host_v1_fixture_probe"},
  };
  std::string error;
  check(session.setRuntimeSymbolMappings(runtime_symbols, error));
  if (!session.compile(error))
    std::fprintf(stderr, "stdlib LLVM fixture compile failed: %s\n",
                 error.c_str());
  check(error.empty());
  const auto llvm = session.unit(consumer).printLLVM();
  check(!llvm.empty());
  check(llvm.find("chtholly_next_host_v1_fixture_probe") !=
        std::string::npos);
  check(llvm.find("@fixture_probe") == std::string::npos);
  check(session.exportInteropBundle(sidecar.string(), error));

  ArtifactBundle bundle;
  check(chtholly::compiler::interop::readArtifactBundle(sidecar.string(), bundle,
                                                     error));
  bool saw_multi_event = false;
  bool saw_host_outcome = false;
  for (const auto &record : bundle.records) {
    if (record.reference.canonical_module != "std::host")
      continue;
    if (record.artifact.completion_events.size() >= 3 &&
        record.artifact.cancel_events.size() == 1 &&
        record.artifact.wake_events.size() == 1) {
      saw_multi_event = true;
    }
    if (record.reference.canonical_name == "host_open" &&
        record.artifact.status_lane == 3 &&
        record.artifact.status_success_literal == 0 &&
        record.artifact.out_initialized_on_failure &&
        record.artifact.out_lanes == std::vector<std::uint32_t>{2})
      saw_host_outcome = true;
  }
  check(saw_multi_event);
  check(saw_host_outcome);
  std::filesystem::remove_all(root, cleanup_error);
  return 0;
}
