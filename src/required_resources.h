#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <cereal/access.hpp>
#include <cereal/types/variant.hpp>

#include "ast/location.h"
#include "format_string.h"
#include "globalvars.h"
#include "map_info.h"
#include "struct.h"
#include "types.h"
#include "util/bpf_funcs.h"

namespace bpftrace {

class BPFtrace;

static const auto DIVIDE_BY_ZERO_MSG =
    "Divide or modulo by 0 detected. This can lead to unexpected "
    "results. 1 is being used as the result.";

enum class RuntimeErrorId {
  DIVIDE_BY_ZERO,
  HELPER_ERROR,
};

enum class PrintfSeverity {
  NONE,
  ERROR,
};

struct SourceLocation {
  std::string filename;
  int line;
  int column;
  std::string source_location;
  std::vector<std::string> source_context;

private:
  friend class cereal::access;
  template <typename Archive>
  void serialize(Archive &archive)
  {
    archive(filename, line, column, source_location, source_context);
  }
};

class SourceInfo {
public:
  SourceInfo(const ast::Location &loc)
  {
    auto curr_loc = loc;

    while (curr_loc) {
      locations.emplace_back(curr_loc->filename(),
                             curr_loc->line(),
                             curr_loc->column(),
                             curr_loc->source_location(),
                             curr_loc->source_context());
      auto &parent = curr_loc->parent;
      if (parent) {
        curr_loc = parent->loc;
      } else {
        break;
      }
    }
  }

  SourceInfo() = default;

  std::vector<SourceLocation> locations;

private:
  friend class cereal::access;
  template <typename Archive>
  void serialize(Archive &archive)
  {
    archive(locations);
  }
};

class RuntimeErrorInfo : public SourceInfo {
public:
  // This class effectively wraps a location, but preserves only the parts that
  // are needed to emit the error in a useful way. This is because it may be
  // serialized and used by a separate runtime.
  RuntimeErrorInfo(RuntimeErrorId error_id,
                   libbpf::bpf_func_id func_id,
                   const ast::Location &loc)
      : SourceInfo(loc), error_id(error_id), func_id(func_id)
  {
  }

  RuntimeErrorInfo(RuntimeErrorId error_id, const ast::Location &loc)
      : RuntimeErrorInfo(error_id, static_cast<libbpf::bpf_func_id>(-1), loc) {
        };

  RuntimeErrorInfo()
      : error_id(RuntimeErrorId::HELPER_ERROR),
        func_id(static_cast<libbpf::bpf_func_id>(-1)) {};

  RuntimeErrorId error_id;
  libbpf::bpf_func_id func_id;

private:
  friend class cereal::access;
  template <typename Archive>
  void serialize(Archive &archive)
  {
    archive(error_id, func_id);
  }
};

std::ostream &operator<<(std::ostream &os, const RuntimeErrorInfo &info);

// This class contains script-specific metadata that bpftrace's runtime needs.
//
// This class is intended to completely encapsulate all of a script's runtime
// needs such as maps, async printf argument metadata, etc. An instance of this
// class plus the actual bpf bytecode should be all that's necessary to run a
// script on another host.
class RequiredResources {
public:
  // `save_state()` serializes `RequiredResources` and writes results into
  // `out`. `load_state()` does the reverse: takes serialized data and loads it
  // into the current instance.
  //
  // NB: The serialized data is not versioned and is not forward/backwards
  // compatible.
  //
  // NB: both the output and input stream must be opened in binary
  // (std::ios::binary) mode to avoid binary data from being interpreted wrong
  void save_state(std::ostream &out) const;
  void load_state(std::istream &in);
  void load_state(const uint8_t *ptr, size_t len);

  // Async argument metadata
  std::vector<
      std::tuple<FormatString, std::vector<Field>, PrintfSeverity, SourceInfo>>
      printf_args;
  std::vector<std::tuple<FormatString, std::vector<Field>>> system_args;
  // fmt strings for BPF helpers (bpf_seq_printf, bpf_trace_printk)
  std::vector<FormatString> bpf_print_fmts;
  std::vector<std::tuple<FormatString, std::vector<Field>>> cat_args;
  std::vector<std::string> join_args;
  std::vector<std::string> time_args;
  std::vector<std::string> strftime_args;
  std::vector<std::string> cgroup_path_args;
  std::vector<SizedType> non_map_print_args;
  std::vector<std::tuple<std::string, long>> skboutput_args_;
  // While max fmtstring args size is not used at runtime, the size
  // calculation requires taking into account struct alignment semantics,
  // and that is tricky enough that we want to minimize repetition of
  // such logic in the codebase. So keep it in resource analysis
  // rather than duplicating it in CodegenResources.
  uint64_t max_fmtstring_args_size = 0;

  // Required for sizing of tuple scratch buffer
  size_t tuple_buffers = 0;
  size_t max_tuple_size = 0;

  // Required for sizing of string scratch buffer
  size_t str_buffers = 0;

  // Required for sizing of map value scratch buffers
  size_t read_map_value_buffers = 0;
  size_t max_read_map_value_size = 0;
  size_t max_write_map_value_size = 0;

  // Required for sizing of variable scratch buffers
  size_t variable_buffers = 0;
  size_t max_variable_size = 0;

  // Required for sizing of map key scratch buffers
  size_t map_key_buffers = 0;
  size_t max_map_key_size = 0;

  // Async argument metadata that codegen creates. Ideally ResourceAnalyser
  // pass should be collecting this, but it's complex to move the logic.
  //
  // Don't add more async arguments here!.
  std::unordered_map<int64_t, RuntimeErrorInfo> runtime_error_info;
  std::vector<std::string> probe_ids;

  // Map metadata
  std::map<std::string, MapInfo> maps_info;
  globalvars::GlobalVars global_vars;
  bool using_skboutput = false;

  // Probe metadata
  //
  // Probe metadata that codegen creates. Ideally ResourceAnalyser pass should
  // be collecting this, but it's complex to move the logic.
  std::vector<Probe> probes;
  std::unordered_map<std::string, Probe> special_probes;
  std::vector<Probe> benchmark_probes;
  std::vector<Probe> signal_probes;
  std::vector<Probe> watchpoint_probes;

  // List of probes using userspace symbol resolution
  std::unordered_set<const ast::Probe *> probes_using_usym;

private:
  friend class cereal::access;
  template <typename Archive>
  void serialize(Archive &archive)
  {
    archive(system_args,
            bpf_print_fmts,
            join_args,
            time_args,
            strftime_args,
            cat_args,
            non_map_print_args,
            runtime_error_info,
            printf_args,
            probe_ids,
            maps_info,
            global_vars,
            using_skboutput,
            probes,
            signal_probes,
            special_probes,
            benchmark_probes);
  }
};

} // namespace bpftrace
