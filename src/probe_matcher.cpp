#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "bpftrace.h"
#include "btf.h"
#include "cxxdemangler/cxxdemangler.h"
#include "dwarf_parser.h"
#include "log.h"
#include "probe_matcher.h"
#include "scopeguard.h"
#include "tracefs/tracefs.h"
#include "util/bpf_progs.h"
#include "util/paths.h"
#include "util/strings.h"
#include "util/symbols.h"
#include "util/system.h"
#include "util/wildcard.h"

#include <bcc/bcc_elf.h>
#include <bcc/bcc_syms.h>
#include <elf.h>

namespace bpftrace {

static int add_symbol(const char* symname,
                      uint64_t /*start*/,
                      uint64_t /*size*/,
                      void* payload)
{
  auto* syms = static_cast<std::set<std::string>*>(payload);
  syms->insert(std::string(symname));
  return 0;
}

// Finds all matches of search_input in the provided input stream.
std::set<std::string> ProbeMatcher::get_matches_in_stream(
    const std::string& search_input,
    std::istream& symbol_stream,
    bool demangle_symbols,
    const char delim)
{
  bool start_wildcard, end_wildcard;
  auto tokens = util::get_wildcard_tokens(search_input,
                                          start_wildcard,
                                          end_wildcard);

  // Since demangled_name contains function parameters, we need to remove
  // them unless the user specified '(' in the search input (i.e. wants
  // to match against the parameters explicitly).
  // Only used for C++ when demangling is enabled.
  auto has_parameter = [](const std::string& token) {
    return token.find('(') != std::string::npos;
  };
  const bool truncate_parameters = std::ranges::none_of(tokens,

                                                        has_parameter);

  std::string line;
  std::set<std::string> matches;
  while (std::getline(symbol_stream, line, delim)) {
    if (!util::wildcard_match(line, tokens, start_wildcard, end_wildcard)) {
      if (demangle_symbols) {
        auto fun_line = line;
        auto prefix = fun_line.find(':') != std::string::npos
                          ? util::erase_prefix(fun_line) + ":"
                          : "";
        if (util::symbol_has_cpp_mangled_signature(fun_line)) {
          char* demangled_name = cxxdemangle(fun_line.c_str());
          if (!demangled_name)
            continue;
          SCOPE_EXIT
          {
            ::free(demangled_name);
          };

          // Match against the demanled name.
          std::string match_line = prefix + demangled_name;
          if (truncate_parameters) {
            util::erase_parameter_list(match_line);
          }

          if (util::wildcard_match(
                  match_line, tokens, start_wildcard, end_wildcard)) {
            goto out;
          }
        }
      }
      continue;
    }
  out:
    // skip the ".part.N" kprobe variants, as they can't be traced:
    if (line.find(".part.") != std::string::npos)
      continue;

    matches.insert(line);
  }
  return matches;
}

// Get matches of search_input (containing a wildcard) for a given probe_type.
// probe_type determines where to take the candidate matches from.
// Some probe types (e.g. uprobe) require target to be specified.
std::set<std::string> ProbeMatcher::get_matches_for_probetype(
    const ProbeType& probe_type,
    const std::string& target,
    const std::string& search_input,
    bool demangle_symbols)
{
  std::unique_ptr<std::istream> symbol_stream;

  switch (probe_type) {
    case ProbeType::kprobe:
    case ProbeType::kretprobe: {
      if (!target.empty())
        symbol_stream = get_symbols_from_traceable_funcs(true);
      else
        symbol_stream = get_symbols_from_traceable_funcs(false);
      break;
    }
    case ProbeType::uprobe:
    case ProbeType::uretprobe:
    case ProbeType::watchpoint:
    case ProbeType::asyncwatchpoint: {
      symbol_stream = get_func_symbols_from_file(bpftrace_->pid(), target);
      break;
    }
    case ProbeType::tracepoint: {
      symbol_stream = get_symbols_from_file(tracefs::available_events());
      break;
    }
    // The two `has_btf_data` checks below for fentry/fexit/rawtracepoints
    // are more about ordering than system properties.
    // Initially, before BTF is loaded and we want to determine what modules to
    // load BTF from we check "/sys/kernel/tracing/available_filter_functions".
    // Then we check the BTF to filter out functions from that list that don't
    // have any BTF definitions.
    case ProbeType::rawtracepoint: {
      symbol_stream = get_raw_tracepoint_symbols();
      break;
    }
    case ProbeType::fentry:
    case ProbeType::fexit: {
      if (target == "bpf") {
        symbol_stream = get_running_bpf_programs();
      } else {
        symbol_stream = get_fentry_symbols();
      }
      break;
    }
    case ProbeType::usdt: {
      symbol_stream = get_symbols_from_usdt(bpftrace_->pid(), target);
      break;
    }
    case ProbeType::software: {
      symbol_stream = get_symbols_from_list(SW_PROBE_LIST);
      break;
    }
    case ProbeType::hardware: {
      symbol_stream = get_symbols_from_list(HW_PROBE_LIST);
      break;
    }
    case ProbeType::iter: {
      if (!bpftrace_->has_btf_data())
        break;

      std::string ret;
      auto iters = bpftrace_->btf_->get_all_iters();
      for (const auto& iter : iters) {
        // second check
        if (bpftrace_->feature_->has_iter(iter))
          ret += iter + "\n";
        else
          LOG(WARNING) << "The kernel contains bpf_iter__" << iter
                       << " struct but does not support loading an iterator"
                          " program against it. Please report this bug.";
      }
      symbol_stream = std::make_unique<std::istringstream>(ret);
      break;
    }
    case ProbeType::interval:
    case ProbeType::profile: {
      std::string ret;
      for (const auto& unit : TIME_UNITS)
        ret += unit + ":\n";
      symbol_stream = std::make_unique<std::istringstream>(ret);
      break;
    }
    case ProbeType::special:
    case ProbeType::benchmark:
      return { target + ":" };
    default:
      return {};
  }

  if (symbol_stream)
    return get_matches_in_stream(search_input,
                                 *symbol_stream,
                                 demangle_symbols);
  else
    return {};
}

// Find all matches of search_input in set
std::set<std::string> ProbeMatcher::get_matches_in_set(
    const std::string& search_input,
    const std::set<std::string>& set)
{
  std::string stream_in;
  // Strings in the set may contain a newline character, so we use '$'
  // as a delimiter.
  for (const auto& str : set)
    stream_in.append(str + "$");

  std::istringstream stream(stream_in);
  return get_matches_in_stream(search_input, stream, false, '$');
}

std::unique_ptr<std::istream> ProbeMatcher::get_symbols_from_file(
    const std::string& path) const
{
  auto file = std::make_unique<std::ifstream>(path);
  if (file->fail()) {
    LOG(WARNING) << "Could not read symbols from " << path << ": "
                 << strerror(errno);
    return nullptr;
  }

  return file;
}

std::unique_ptr<std::istream> ProbeMatcher::get_symbols_from_traceable_funcs(
    bool with_modules) const
{
  std::string funcs;
  for (const auto& func_mod : bpftrace_->get_traceable_funcs()) {
    if (with_modules) {
      for (const auto& mod : func_mod.second)
        funcs += mod + ":" + func_mod.first + "\n";
    } else {
      funcs += func_mod.first + "\n";
    }
  }
  return std::make_unique<std::istringstream>(funcs);
}

std::unique_ptr<std::istream> ProbeMatcher::get_fentry_symbols() const
{
  if (bpftrace_->btf_->has_data() && bpftrace_->btf_->modules_loaded())
    return bpftrace_->btf_->get_all_funcs();
  else {
    return get_symbols_from_traceable_funcs(true);
  }
}

std::unique_ptr<std::istream> ProbeMatcher::get_running_bpf_programs() const
{
  std::string funcs;
  auto ids_and_syms = util::get_bpf_progs();
  for (const auto& [id, symbol] : ids_and_syms) {
    funcs += "bpf:" + std::to_string(id) + ":" + symbol + "\n";
  }
  return std::make_unique<std::istringstream>(funcs);
}

std::unique_ptr<std::istream> ProbeMatcher::get_raw_tracepoint_symbols() const
{
  if (bpftrace_->btf_->has_data() && bpftrace_->btf_->modules_loaded()) {
    return bpftrace_->btf_->get_all_raw_tracepoints();
  } else {
    std::string rts;
    for (const auto& rt_mod : bpftrace_->get_raw_tracepoints()) {
      for (const auto& mod : rt_mod.second)
        rts += mod + ":" + rt_mod.first + "\n";
    }
    return std::make_unique<std::istringstream>(rts);
  }
}

std::unique_ptr<std::istream> ProbeMatcher::get_func_symbols_from_file(
    std::optional<int> pid,
    const std::string& path) const
{
  if (path.empty())
    return std::make_unique<std::istringstream>("");

  std::vector<std::string> real_paths;
  if (path == "*") {
    if (pid.has_value())
      real_paths = util::get_mapped_paths_for_pid(*pid);
    else
      real_paths = util::get_mapped_paths_for_running_pids();
  } else if (path.find('*') != std::string::npos)
    real_paths = util::resolve_binary_path(path, pid);
  else
    real_paths.push_back(path);
  struct bcc_symbol_option symbol_option;
  memset(&symbol_option, 0, sizeof(symbol_option));
  symbol_option.use_debug_file = 1;
  symbol_option.check_debug_file_crc = 1;
  symbol_option.use_symbol_type = (1 << STT_FUNC) | (1 << STT_GNU_IFUNC);

  std::string result;
  for (auto& real_path : real_paths) {
    std::set<std::string> syms;
    // Workaround: bcc_elf_foreach_sym() can return the same symbol twice if
    // it's also found in debug info (#1138), so a std::set is used here (and in
    // the add_symbol callback) to ensure that each symbol will be unique in the
    // returned string.
    int err = bcc_elf_foreach_sym(
        real_path.c_str(), add_symbol, &symbol_option, &syms);
    if (err) {
      LOG(WARNING) << "Could not list function symbols: " + real_path;
    }
    for (const auto& sym : syms)
      result += real_path + ":" + sym + "\n";
  }
  return std::make_unique<std::istringstream>(result);
}

std::unique_ptr<std::istream> ProbeMatcher::get_symbols_from_usdt(
    std::optional<int> pid,
    const std::string& target) const
{
  std::string probes;
  usdt_probe_list usdt_probes;

  if (pid.has_value())
    usdt_probes = USDTHelper::probes_for_pid(*pid);
  else if (target == "*")
    usdt_probes = USDTHelper::probes_for_all_pids();
  else if (!target.empty()) {
    std::vector<std::string> real_paths;
    if (target.find('*') != std::string::npos)
      real_paths = util::resolve_binary_path(target);
    else
      real_paths.push_back(target);

    for (auto& real_path : real_paths) {
      auto target_usdt_probes = USDTHelper::probes_for_path(real_path);
      usdt_probes.insert(usdt_probes.end(),
                         target_usdt_probes.begin(),
                         target_usdt_probes.end());
    }
  }

  for (auto const& usdt_probe : usdt_probes) {
    std::string path = usdt_probe.path;
    std::string provider = usdt_probe.provider;
    std::string fname = usdt_probe.name;
    probes += path + ":" + provider + ":" + fname + "\n";
  }

  return std::make_unique<std::istringstream>(probes);
}

std::unique_ptr<std::istream> ProbeMatcher::get_symbols_from_list(
    const std::vector<ProbeListItem>& probes_list) const
{
  std::string symbols;
  for (const auto& probe : probes_list) {
    symbols += probe.path + ":\n";
    if (!probe.alias.empty())
      symbols += probe.alias + ":\n";
  }
  return std::make_unique<std::istringstream>(symbols);
}

// Get list of kernel probe types for the purpose of listing.
// Ignore return probes and aliases.
std::unique_ptr<std::istream> ProbeMatcher::kernel_probe_list()
{
  std::string probes;
  for (const auto& p : PROBE_LIST) {
    if (!p.show_in_kernel_list) {
      continue;
    }
    if (p.type == ProbeType::fentry) {
      // fentry must be available
      if (bpftrace_->feature_->has_fentry())
        probes += p.name + "\n";
    } else {
      probes += p.name + "\n";
    }
  }

  return std::make_unique<std::istringstream>(probes);
}

// Get list of userspace probe types for the purpose of listing.
// Ignore return probes.
std::unique_ptr<std::istream> ProbeMatcher::userspace_probe_list()
{
  std::string probes;
  for (const auto& p : PROBE_LIST) {
    if (p.show_in_userspace_list) {
      probes += p.name + "\n";
    }
  }

  return std::make_unique<std::istringstream>(probes);
}

FuncParamLists ProbeMatcher::get_tracepoints_params(
    const std::set<std::string>& tracepoints)
{
  FuncParamLists params;
  for (const auto& tracepoint : tracepoints) {
    auto event = tracepoint;
    auto category = util::erase_prefix(event);

    std::string format_file_path = tracefs::event_format_file(category, event);
    std::ifstream format_file(format_file_path.c_str());
    std::string line;

    if (format_file.fail()) {
      LOG(ERROR) << "tracepoint format file not found: " << format_file_path;
      return {};
    }

    // Skip lines until the first empty line
    do {
      getline(format_file, line);
    } while (!line.empty());

    while (getline(format_file, line)) {
      if (line.starts_with("\tfield:")) {
        size_t col_pos = line.find(':') + 1;
        params[tracepoint].push_back(
            line.substr(col_pos, line.find(';') - col_pos));
      }
    }
  }
  return params;
}

FuncParamLists ProbeMatcher::get_iters_params(
    const std::set<std::string>& iters)
{
  const std::string prefix = "vmlinux:bpf_iter_";
  FuncParamLists params;
  std::set<std::string> funcs;

  for (const auto& iter : iters)
    funcs.insert(prefix + iter);

  params = bpftrace_->btf_->get_params(funcs);
  for (auto func : funcs) {
    // delete `int retval`
    params[func].pop_back();
    // delete `struct bpf_iter_meta * meta`
    params[func].erase(params[func].begin());
    // restore key value
    auto param = params.extract(func);
    param.key() = func.substr(prefix.size());
    params.insert(std::move(param));
  }
  return params;
}

FuncParamLists ProbeMatcher::get_uprobe_params(
    const std::set<std::string>& uprobes)
{
  FuncParamLists params;
  static std::set<std::string> warned_paths;

  for (const auto& match : uprobes) {
    std::string fun = match;
    std::string path = util::erase_prefix(fun);
    auto dwarf = Dwarf::GetFromBinary(nullptr, path);
    if (dwarf)
      params.emplace(match, dwarf->get_function_params(fun));
    else {
      if (warned_paths.insert(path).second)
        LOG(WARNING) << "No DWARF found for \"" << path << "\""
                     << ", cannot show parameter info";
    }
  }

  return params;
}

void ProbeMatcher::list_probes(ast::Program* prog)
{
  for (auto* probe : prog->probes) {
    for (auto* ap : probe->attach_points) {
      auto matches = get_matches_for_ap(*ap);
      auto probe_type = probetype(ap->provider);
      FuncParamLists param_lists;
      if (bt_verbose) {
        if (probe_type == ProbeType::tracepoint)
          param_lists = get_tracepoints_params(matches);
        else if (probe_type == ProbeType::fentry ||
                 probe_type == ProbeType::fexit)
          param_lists = bpftrace_->btf_->get_params(matches);
        else if (probe_type == ProbeType::rawtracepoint)
          param_lists = bpftrace_->btf_->get_rawtracepoint_params(matches);
        else if (probe_type == ProbeType::iter)
          param_lists = get_iters_params(matches);
        else if (probe_type == ProbeType::uprobe)
          param_lists = get_uprobe_params(matches);
        else if (probe_type == ProbeType::kprobe)
          param_lists = bpftrace_->btf_->get_kprobes_params(matches);
        else if (probe_type == ProbeType::kretprobe)
          param_lists = bpftrace_->btf_->get_kretprobes_params(matches);
      }

      for (const auto& match : matches) {
        std::string match_print = match;
        if (ap->lang == "cpp") {
          std::string target = util::erase_prefix(match_print);
          char* demangled_name = cxxdemangle(match_print.c_str());
          SCOPE_EXIT
          {
            ::free(demangled_name);
          };

          // demangled name may contain symbols not accepted by the attach point
          // parser, so surround it with quotes to make the entry directly
          // usable as an attach point
          auto func = demangled_name ? "\"" + std::string(demangled_name) + "\""
                                     : match_print;

          match_print = target + ":" + ap->lang + ":" + func;
        }

        std::cout << probe_type << ":" << match_print << std::endl;
        if (bt_verbose) {
          for (auto& param : param_lists[match])
            std::cout << "    " << param << std::endl;
        }
      }
    }
  }
}

std::set<std::string> ProbeMatcher::get_matches_for_ap(
    const ast::AttachPoint& attach_point)
{
  std::string search_input;
  switch (probetype(attach_point.provider)) {
    case ProbeType::kprobe:
    case ProbeType::kretprobe: {
      if (!attach_point.target.empty())
        search_input = attach_point.target + ":" + attach_point.func;
      else
        search_input = attach_point.func;
      break;
    }
    case ProbeType::rawtracepoint: {
      search_input = attach_point.target + ":" + attach_point.func;
      break;
    }
    case ProbeType::iter: {
      search_input = attach_point.func;
      break;
    }
    case ProbeType::fentry:
    case ProbeType::fexit: {
      if (attach_point.target == "bpf") {
        search_input = "bpf:" +
                       (attach_point.bpf_prog_id
                            ? std::to_string(attach_point.bpf_prog_id)
                            : "*") +
                       ":" + attach_point.func;
        break;
      }
      [[fallthrough]];
    }
    case ProbeType::special:
    case ProbeType::benchmark:
    case ProbeType::uprobe:
    case ProbeType::uretprobe:
    case ProbeType::watchpoint:
    case ProbeType::asyncwatchpoint:
    case ProbeType::tracepoint: {
      // Do not expand "target:" as that would match all functions in target.
      // This may occur when an absolute address is given instead of a function.
      if (attach_point.func.empty())
        return { attach_point.target + ":" };

      search_input = attach_point.target + ":" + attach_point.func;
      break;
    }
    case ProbeType::hardware:
    case ProbeType::software:
    case ProbeType::profile:
    case ProbeType::interval: {
      search_input = attach_point.target + ":";
      break;
    }
    case ProbeType::usdt: {
      auto target = attach_point.target;
      // If PID is specified, targets in symbol_stream will have the
      // "/proc/<PID>/root" prefix followed by an absolute path, so we make the
      // target absolute and add a leading wildcard.
      if (bpftrace_->pid().has_value()) {
        if (!target.empty()) {
          if (auto abs_target = util::abs_path(target))
            target = "*" + abs_target.value();
        } else
          target = "*";
      }
      auto ns = attach_point.ns.empty() ? "*" : attach_point.ns;
      search_input = target + ":" + ns + ":" + attach_point.func;
      break;
    }
    case ProbeType::invalid:
      throw WildcardException(
          "Wildcard matches aren't available on probe type '" +
          attach_point.provider + "'");
  }

  return get_matches_for_probetype(probetype(attach_point.provider),
                                   attach_point.target,
                                   search_input,
                                   attach_point.lang == "cpp");
}

std::set<std::string> ProbeMatcher::expand_probetype_kernel(
    const std::string& probe_type)
{
  if (util::has_wildcard(probe_type))
    return get_matches_in_stream(probe_type, *kernel_probe_list());
  else
    return { probe_type };
}

std::set<std::string> ProbeMatcher::expand_probetype_userspace(
    const std::string& probe_type)
{
  if (util::has_wildcard(probe_type))
    return get_matches_in_stream(probe_type, *userspace_probe_list());
  else
    return { probe_type };
}

void ProbeMatcher::list_structs(const std::string& search)
{
  auto structs = bpftrace_->btf_->get_all_structs();

  std::string search_input = search;
  // If verbose is on, structs will contain full definitions
  if (bt_verbose)
    search_input += " *{*}*";

  for (const auto& match : get_matches_in_set(search_input, structs))
    std::cout << match << std::endl;
}

} // namespace bpftrace
