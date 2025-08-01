#include <algorithm>
#include <bcc/bcc_proc.h>
#include <cctype>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "ast/attachpoint_parser.h"
#include "ast/context.h"
#include "ast/helpers.h"
#include "types.h"
#include "util/int_parser.h"
#include "util/paths.h"
#include "util/strings.h"
#include "util/system.h"
#include "util/wildcard.h"

namespace bpftrace::ast {

AttachPointParser::State AttachPointParser::argument_count_error(
    int expected,
    std::optional<int> expected2)
{
  // Subtract one for the probe type (eg kprobe)
  int found = parts_.size() - 1;

  errs_ << ap_->provider << " probe type requires " << expected;
  if (expected2.has_value()) {
    errs_ << " or " << *expected2;
  }
  errs_ << " arguments, found " << found << std::endl;

  return INVALID;
}

AttachPointParser::AttachPointParser(ASTContext &ctx,
                                     BPFtrace &bpftrace,
                                     bool listing)
    : ctx_(ctx), bpftrace_(bpftrace), listing_(listing)
{
}

int AttachPointParser::parse()
{
  if (!ctx_.root)
    return 1;

  uint32_t failed = 0;
  for (Probe *probe : ctx_.root->probes) {
    for (size_t i = 0; i < probe->attach_points.size(); ++i) {
      auto *ap_ptr = probe->attach_points[i];
      auto &ap = *ap_ptr;
      new_attach_points.clear();

      State s = parse_attachpoint(ap);
      if (s == INVALID) {
        ++failed;
        ap.addError() << errs_.str();
      } else if (s == SKIP || s == NEW_APS) {
        // Remove the current attach point
        probe->attach_points.erase(probe->attach_points.begin() + i);
        i--;
        if (s == NEW_APS) {
          // The removed attach point is replaced by new ones
          probe->attach_points.insert(probe->attach_points.end(),
                                      new_attach_points.begin(),
                                      new_attach_points.end());
        }
      }

      // clear error buffer between attach points to prevent non-fatal errors
      // from being carried over and printed on the next fatal error
      errs_.str({});
    }

    auto it = std::ranges::remove_if(probe->attach_points,
                                     [](const AttachPoint *ap) {
                                       return ap->provider.empty();
                                     });
    probe->attach_points.erase(it.begin(), it.end());

    if (probe->attach_points.empty()) {
      probe->addError() << "No attach points for probe";
      failed++;
    }
  }

  return failed;
}

AttachPointParser::State AttachPointParser::parse_attachpoint(AttachPoint &ap)
{
  ap_ = &ap;

  parts_.clear();
  if (State s = lex_attachpoint(*ap_))
    return s;

  if (parts_.empty()) {
    errs_ << "Invalid attachpoint definition" << std::endl;
    return INVALID;
  }

  if (parts_.front().empty()) {
    // Do not fail on empty attach point, could be just a trailing comma
    ap_->provider = "";
    return OK;
  }

  std::set<std::string> probe_types;
  if (util::has_wildcard(parts_.front())) {
    // Single argument listing looks at all relevant probe types
    std::string probetype_query = (parts_.size() == 1) ? "*" : parts_.front();

    // Probe type expansion
    // If PID is specified or the second part of the attach point is a path
    // (contains '/'), use userspace probe types.
    // Otherwise, use kernel probe types.
    if (bpftrace_.pid().has_value() ||
        (parts_.size() >= 2 && parts_[1].find('/') != std::string::npos)) {
      probe_types = bpftrace_.probe_matcher_->expand_probetype_userspace(
          probetype_query);
    } else {
      probe_types = bpftrace_.probe_matcher_->expand_probetype_kernel(
          probetype_query);
    }
  } else
    probe_types = { parts_.front() };

  if (probe_types.empty()) {
    if (util::has_wildcard(parts_.front()))
      errs_ << "No probe type matched for " << parts_.front() << std::endl;
    else
      errs_ << "Invalid probe type: " << parts_.front() << std::endl;
    return INVALID;
  } else if (probe_types.size() > 1) {
    // If the probe type string matches more than 1 probe, create a new set of
    // attach points (one for every match) that will replace the original one.
    for (const auto &probe_type : probe_types) {
      std::string raw_input = ap.raw_input;
      if (parts_.size() > 1)
        util::erase_prefix(raw_input);
      raw_input = probe_type + ":" + raw_input;
      // New attach points have ignore_invalid set to true - probe types for
      // which raw_input has invalid number of parts will be ignored (instead
      // of throwing an error). These will have the same associated location.
      new_attach_points.push_back(
          ctx_.make_node<AttachPoint>(raw_input, true, Location(ap.loc)));
    }
    return NEW_APS;
  }

  ap.provider = expand_probe_name(*probe_types.begin());

  switch (probetype(ap.provider)) {
    case ProbeType::special:
      return special_parser();
    case ProbeType::benchmark:
      return benchmark_parser();
    case ProbeType::kprobe:
      return kprobe_parser();
    case ProbeType::kretprobe:
      return kretprobe_parser();
    case ProbeType::uprobe:
      return uprobe_parser();
    case ProbeType::uretprobe:
      return uretprobe_parser();
    case ProbeType::usdt:
      return usdt_parser();
    case ProbeType::tracepoint:
      return tracepoint_parser();
    case ProbeType::profile:
      return profile_parser();
    case ProbeType::interval:
      return interval_parser();
    case ProbeType::software:
      return software_parser();
    case ProbeType::hardware:
      return hardware_parser();
    case ProbeType::watchpoint:
      return watchpoint_parser();
    case ProbeType::asyncwatchpoint:
      return watchpoint_parser(true);
    case ProbeType::fentry:
    case ProbeType::fexit:
      return fentry_parser();
    case ProbeType::iter:
      return iter_parser();
    case ProbeType::rawtracepoint:
      return raw_tracepoint_parser();
    case ProbeType::invalid:
      errs_ << "Invalid probe type: " << ap.provider << std::endl;
      return INVALID;
  }

  __builtin_unreachable();
}

AttachPointParser::State AttachPointParser::lex_attachpoint(
    const AttachPoint &ap)
{
  std::string raw = ap.raw_input;
  std::vector<std::string> ret;
  bool in_quotes = false;
  std::string argument;

  for (size_t idx = 0; idx < raw.size(); ++idx) {
    if (raw[idx] == ':' && !in_quotes) {
      parts_.emplace_back(std::move(argument));
      // The standard says an std::string in moved-from state is in
      // valid but unspecified state, so clear() to be safe
      argument.clear();
    } else if (raw[idx] == '"')
      in_quotes = !in_quotes;
    // Handle escaped characters in a string
    else if (in_quotes && raw[idx] == '\\' && (idx + 1 < raw.size())) {
      argument += raw[idx + 1];
      ++idx;
    } else if (!in_quotes && raw[idx] == '$') {
      size_t i = idx + 1;
      size_t len = 0;
      while (i < raw.size() && std::isdigit(raw[i])) {
        if (len == 0 && raw[i] == '0') {
          break;
        }
        len++;
        i++;
      }

      std::string param_idx_str;

      if (len == 0 && (idx + 1) < raw.size()) {
        param_idx_str = raw.substr(idx + 1, 1);
        errs_
            << "invalid trailing character for positional param: "
            << param_idx_str
            << ". Try quoting this entire part if this is intentional e.g. \"$"
            << param_idx_str << "\".";
        return State::INVALID;
      }

      param_idx_str = raw.substr(idx + 1, len);
      auto param_idx = util::to_uint(param_idx_str, 10);
      if (!param_idx) {
        errs_ << "positional parameter is not valid: " << param_idx.takeError()
              << std::endl;
        return State::INVALID;
      }

      // Expand the positional param in-place and decrement idx so that the next
      // iteration takes the first char of the expansion
      raw = raw.substr(0, idx) + bpftrace_.get_param(*param_idx) +
            raw.substr(i);
      idx--;
    } else
      argument += raw[idx];
  }

  // Add final argument
  //
  // There will always be text in `argument` unless the AP definition
  // ended in a ':' which we will treat as an empty argument.
  parts_.emplace_back(std::move(argument));

  return State::OK;
}

AttachPointParser::State AttachPointParser::special_parser()
{
  // Can only have reached here if provider is `begin` or `end` or `self`
  assert(ap_->provider == "begin" || ap_->provider == "end" ||
         ap_->provider == "self");

  if (ap_->provider == "begin" || ap_->provider == "end") {
    if (parts_.size() == 2 && parts_[1] == "*")
      parts_.pop_back();
    if (parts_.size() != 1) {
      return argument_count_error(0);
    }
  } else if (ap_->provider == "self") {
    if (parts_.size() != 3) {
      return argument_count_error(2);
    }
    ap_->target = parts_[1];
    ap_->func = parts_[2];
  }

  return OK;
}

AttachPointParser::State AttachPointParser::benchmark_parser()
{
  // Can only have reached here if provider is `bench`
  assert(ap_->provider == "bench");

  if (ap_->provider == "bench") {
    if (parts_.size() != 2) {
      return argument_count_error(1);
    }

    ap_->target = parts_[1];
  }

  return OK;
}

AttachPointParser::State AttachPointParser::kprobe_parser(bool allow_offset)
{
  auto num_parts = parts_.size();
  if (num_parts != 2 && num_parts != 3) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(1, 2);
  }

  auto func_idx = 1;
  if (num_parts == 3) {
    ap_->target = parts_[1];
    func_idx = 2;
  }

  // Handle kprobe:func+0x100 case
  auto plus_count = std::count(parts_[func_idx].cbegin(),
                               parts_[func_idx].cend(),
                               '+');
  if (plus_count) {
    if (!allow_offset) {
      errs_ << "Offset not allowed" << std::endl;
      return INVALID;
    }

    if (plus_count != 1) {
      errs_ << "Cannot take more than one offset" << std::endl;
      return INVALID;
    }

    auto offset_parts = util::split_string(parts_[func_idx], '+', true);
    if (offset_parts.size() != 2) {
      errs_ << "Invalid offset" << std::endl;
      return INVALID;
    }

    ap_->func = offset_parts[0];

    auto res = util::to_uint(offset_parts[1]);
    if (!res) {
      errs_ << "Invalid offset: " << res.takeError() << std::endl;
      return INVALID;
    }
    ap_->func_offset = *res;
  }
  // Default case (eg kprobe:func)
  else {
    ap_->func = parts_[func_idx];
  }

  return OK;
}

AttachPointParser::State AttachPointParser::kretprobe_parser()
{
  return kprobe_parser(false);
}

AttachPointParser::State AttachPointParser::uprobe_parser(bool allow_offset,
                                                          bool allow_abs_addr)
{
  const auto pid = bpftrace_.pid();
  if (pid.has_value() &&
      (parts_.size() == 2 ||
       (parts_.size() == 3 && is_supported_lang(parts_[1])))) {
    // For PID, the target may be skipped
    if (parts_.size() == 2)
      parts_.insert(parts_.begin() + 1, "");

    auto target = util::get_pid_exe(*pid);
    parts_[1] = target ? util::path_for_pid_mountns(*pid, *target) : "";
  }

  if (parts_.size() != 3 && parts_.size() != 4) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(2, 3);
  }

  if (parts_.size() == 4)
    ap_->lang = parts_[2];

  ap_->target = "";

  if (!util::has_wildcard(parts_[1]) && parts_[1].starts_with("lib")) {
    // Automatic resolution of shared library paths.
    // If the target has form "libXXX" then we use BCC to find the correct path
    // to the given library as it may differ across systems.
    auto libname = parts_[1].substr(3);
    auto *lib_path = bcc_procutils_which_so(libname.c_str(),
                                            bpftrace_.pid().value_or(0));
    if (lib_path) {
      ap_->target = lib_path;
      ::free(lib_path);
    }
  }

  if (ap_->target.empty()) {
    ap_->target = parts_[1];
  }

  const std::string &func = parts_.back();
  // Handle uprobe:/lib/asdf:func+0x100 case
  auto plus_count = std::count(func.cbegin(), func.cend(), '+');
  if (plus_count) {
    if (!allow_offset) {
      errs_ << "Offset not allowed" << std::endl;
      return INVALID;
    }

    if (plus_count != 1) {
      errs_ << "Cannot take more than one offset" << std::endl;
      return INVALID;
    }

    auto offset_parts = util::split_string(func, '+', true);
    if (offset_parts.size() != 2) {
      errs_ << "Invalid offset" << std::endl;
      return INVALID;
    }

    ap_->func = offset_parts[0];
    auto res = util::to_uint(offset_parts[1]);
    if (!res) {
      errs_ << "Invalid offset: " << res.takeError() << std::endl;
      return INVALID;
    }
    ap_->func_offset = *res;
  }
  // Default case (eg uprobe:[addr][func])
  else {
    if (allow_abs_addr) {
      auto res = util::to_uint(func);
      if (res) {
        if (util::has_wildcard(ap_->target)) {
          errs_ << "Cannot use wildcards with absolute address" << std::endl;
          return INVALID;
        }
        ap_->address = *res;
      } else {
        ap_->address = 0;
        ap_->func = func;
      }
    } else
      ap_->func = func;
  }

  return OK;
}

AttachPointParser::State AttachPointParser::uretprobe_parser()
{
  return uprobe_parser(false);
}

AttachPointParser::State AttachPointParser::usdt_parser()
{
  if (bpftrace_.pid().has_value()) {
    // For PID, the target can be skipped
    if (parts_.size() == 2) {
      parts_.push_back(parts_[1]);
      parts_[1] = "";
    }
  }
  if (parts_.size() != 3 && parts_.size() != 4) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(2, 3);
  }

  if (parts_.size() == 3) {
    ap_->target = parts_[1];
    ap_->func = parts_[2];
  } else {
    ap_->target = parts_[1];
    ap_->ns = parts_[2];
    ap_->func = parts_[3];
  }

  return OK;
}

AttachPointParser::State AttachPointParser::tracepoint_parser()
{
  // Help with `bpftrace -l 'tracepoint:*foo*'` listing -- wildcard the
  // tracepoint category b/c user is most likely to be looking for the event
  // name
  if (parts_.size() == 2 && util::has_wildcard(parts_.at(1)))
    parts_.insert(parts_.begin() + 1, "*");

  if (parts_.size() != 3) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(2);
  }

  ap_->target = parts_[1];
  ap_->func = parts_[2];

  return OK;
}

// Used for both profile and interval probes
AttachPointParser::State AttachPointParser::frequency_parser()
{
  if (parts_.size() == 2) {
    if (util::has_wildcard(parts_[1])) {
      // Wildcards are allowed for listing
      ap_->target = parts_[1];
      ap_->freq = 0;
      return OK;
    }

    auto res = util::to_uint(parts_[1]);
    if (!res) {
      errs_ << "Invalid rate of " << ap_->provider
            << " probe: " << res.takeError() << std::endl;
      return INVALID;
    }
    if (*res < 1000) {
      errs_ << "Invalid rate of " << ap_->provider
            << " probe. Minimum is 1000 or 1us. Found: " << *res
            << " nanoseconds" << std::endl;
      return INVALID;
    }
    ap_->target = "us";
    // res is in nanoseconds
    ap_->freq = (*res / 1000);
    return OK;
  }

  if (parts_.size() != 3) {
    return argument_count_error(1, 2);
  }

  ap_->target = parts_[1];
  auto res = util::to_uint(parts_[2]);
  if (!res) {
    errs_ << "Invalid rate of " << ap_->provider
          << " probe: " << res.takeError() << std::endl;
    return INVALID;
  }

  ap_->freq = *res;
  return OK;
}

AttachPointParser::State AttachPointParser::profile_parser()
{
  return frequency_parser();
}

AttachPointParser::State AttachPointParser::interval_parser()
{
  return frequency_parser();
}

AttachPointParser::State AttachPointParser::software_parser()
{
  if (parts_.size() != 2 && parts_.size() != 3) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(1, 2);
  }

  ap_->target = parts_[1];

  if (parts_.size() == 3 && parts_[2] != "*") {
    auto res = util::to_uint(parts_[2]);
    if (!res) {
      errs_ << "Invalid count for " << ap_->provider
            << " probe: " << res.takeError() << std::endl;
      return INVALID;
    }
    ap_->freq = *res;
  }

  return OK;
}

AttachPointParser::State AttachPointParser::hardware_parser()
{
  if (parts_.size() != 2 && parts_.size() != 3) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(1, 2);
  }

  ap_->target = parts_[1];

  if (parts_.size() == 3 && parts_[2] != "*") {
    auto res = util::to_uint(parts_[2]);
    if (!res) {
      errs_ << "Invalid count for " << ap_->provider
            << " probe: " << res.takeError() << std::endl;
      return INVALID;
    }
    ap_->freq = *res;
  }

  return OK;
}

AttachPointParser::State AttachPointParser::watchpoint_parser(bool async)
{
  if (parts_.size() != 4) {
    return argument_count_error(3);
  }

  if (parts_[1].find('+') == std::string::npos) {
    auto parsed = util::to_uint(parts_[1]);
    if (!parsed) {
      errs_ << "Invalid function/address argument: " << parsed.takeError()
            << std::endl;
      return INVALID;
    }
    ap_->address = *parsed;
  } else {
    auto func_arg_parts = util::split_string(parts_[1], '+', true);
    if (func_arg_parts.size() != 2) {
      errs_ << "Invalid function/address argument: " << parts_[1] << std::endl;
      return INVALID;
    }

    ap_->func = func_arg_parts[0];

    if (func_arg_parts[1].size() <= 3 ||
        !func_arg_parts[1].starts_with("arg")) {
      errs_ << "Invalid function/address argument: " << func_arg_parts[1]
            << std::endl;
      return INVALID;
    }

    auto parsed = util::to_uint(func_arg_parts[1].substr(3));
    if (!parsed) {
      errs_ << "Invalid function argument: " << parsed.takeError() << std::endl;
      return INVALID;
    }
    ap_->address = *parsed;
  }

  auto len_parsed = util::to_uint(parts_[2]);
  if (!len_parsed) {
    errs_ << "Invalid length argument: " << len_parsed.takeError() << std::endl;
    return INVALID;
  }
  ap_->len = *len_parsed;

  // Semantic analyser will ensure a cmd/pid was provided
  ap_->target = bpftrace_.get_watchpoint_binary_path().value_or("");

  ap_->mode = parts_[3];

  ap_->async = async;

  return OK;
}

AttachPointParser::State AttachPointParser::fentry_parser()
{
  // fentry[:module]:function
  // fentry:bpf:[:prog_id]:prog_name
  if (parts_.size() != 2 && parts_.size() != 3 && parts_.size() != 4) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(1, 3);
  }

  if (parts_[1] == "bpf") {
    ap_->target = parts_[1];
    if (parts_.size() == 2) {
      errs_ << "the 'bpf' variant of this probe requires a bpf program name "
               "and optional bpf program id";
      return INVALID;
    } else if (parts_.size() == 3) {
      ap_->func = parts_[2];
    } else {
      ap_->func = parts_[3];
      if (parts_[2] != "*") {
        auto uint_res = util::to_uint(parts_[2]);
        if (!uint_res) {
          errs_ << "bpf program id must be a number or '*'";
          return INVALID;
        }
        ap_->bpf_prog_id = *uint_res;
      }
    }
    return OK;
  }

  if (parts_.size() == 4) {
    errs_ << "Only the 'bpf' variant of this probe supports 4 arguments";
    return INVALID;
  }

  if (parts_.size() == 3) {
    ap_->target = parts_[1];
    ap_->func = parts_[2];
  } else {
    ap_->func = parts_[1];
    if (!util::has_wildcard(ap_->func)) {
      auto func_modules = bpftrace_.get_func_modules(ap_->func);
      if (func_modules.size() == 1)
        ap_->target = *func_modules.begin();
      else if (func_modules.size() > 1) {
        if (listing_)
          ap_->target = "*";
        else {
          // Attaching to multiple functions of the same name is currently
          // broken, ask the user to specify a module explicitly.
          errs_ << "ambiguous attach point, please specify module containing "
                   "the function \'"
                << ap_->func << "\'";
          return INVALID;
        }
      }
    } else // leave the module empty for now
      ap_->target = "*";
  }

  return OK;
}

AttachPointParser::State AttachPointParser::iter_parser()
{
  if (parts_.size() != 2 && parts_.size() != 3) {
    if (ap_->ignore_invalid)
      return SKIP;

    errs_ << ap_->provider << " probe type takes 2 arguments (1 optional)"
          << std::endl;
    return INVALID;
  }

  ap_->func = parts_[1];

  if (parts_.size() == 3)
    ap_->pin = parts_[2];
  return OK;
}

AttachPointParser::State AttachPointParser::raw_tracepoint_parser()
{
  if (parts_.size() != 2 && parts_.size() != 3) {
    if (ap_->ignore_invalid)
      return SKIP;

    return argument_count_error(2, 1);
  }

  if (parts_.size() == 3) {
    ap_->target = parts_[1];
    ap_->func = parts_[2];
  } else {
    // This is to maintain backwards compatibility with older scripts
    // that couldn't include a target for a raw tracepoint.
    ap_->target = "*";
    ap_->func = parts_[1];
  }

  return OK;
}

// Note: listing changes the parsing semantics for attach points
Pass CreateParseAttachpointsPass(bool listing)
{
  return Pass::create("attachpoints", [listing](ASTContext &ast, BPFtrace &b) {
    AttachPointParser ap_parser(ast, b, listing);
    ap_parser.parse();
  });
}

} // namespace bpftrace::ast
