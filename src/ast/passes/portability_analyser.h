#pragma once

#include <iostream>
#include <sstream>

#include "ast/pass_manager.h"
#include "ast/visitors.h"

namespace bpftrace {
namespace ast {

// Checks if a script uses any non-portable bpftrace features that AOT
// cannot handle.
//
// Over time, we expect to relax these restrictions as AOT supports more
// features.
class PortabilityAnalyser : public Visitor {
public:
  PortabilityAnalyser(ASTContext &ctx, std::ostream &out = std::cerr);
  int analyse();

private:
  void visit(PositionalParameter &param) override;
  void visit(Builtin &builtin) override;
  void visit(Call &call) override;
  void visit(Cast &cast) override;
  void visit(AttachPoint &ap) override;

private:
  std::ostream &out_;
  std::ostringstream err_;
};

Pass CreatePortabilityPass();

} // namespace ast
} // namespace bpftrace
