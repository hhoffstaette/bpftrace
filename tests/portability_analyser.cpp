#include "ast/passes/portability_analyser.h"
#include "ast/attachpoint_parser.h"
#include "ast/passes/field_analyser.h"
#include "ast/passes/map_sugar.h"
#include "ast/passes/named_param.h"
#include "ast/passes/probe_expansion.h"
#include "ast/passes/semantic_analyser.h"
#include "ast/passes/type_system.h"
#include "btf_common.h"
#include "driver.h"
#include "mocks.h"
#include "gtest/gtest.h"

namespace bpftrace::test::portability_analyser {

using ::testing::_;

void test(BPFtrace &bpftrace, const std::string &input, int expected_result = 0)
{
  ast::ASTContext ast("stdin", input);
  std::stringstream msg;
  msg << "\nInput:\n" << input << "\n\nOutput:\n";

  ast::CDefinitions no_c_defs; // Output from clang parser.
  ast::TypeMetadata no_types;  // No external types defined.

  // N.B. No macro expansion.
  auto ok = ast::PassManager()
                .put(ast)
                .put(bpftrace)
                .put(no_c_defs)
                .put(no_types)
                .add(CreateParsePass())
                .add(ast::CreateParseAttachpointsPass())
                .add(ast::CreateProbeExpansionPass())
                .add(ast::CreateMapSugarPass())
                .add(ast::CreateFieldAnalyserPass())
                .add(ast::CreateNamedParamsPass())
                .add(ast::CreateSemanticPass())
                .add(ast::CreatePortabilityPass())
                .run();
  ASSERT_TRUE(bool(ok));
  EXPECT_EQ(int(!ast.diagnostics().ok()), expected_result) << msg.str();
}

void test(const std::string &input, int expected_result = 0)
{
  auto bpftrace = get_mock_bpftrace();
  test(*bpftrace, input, expected_result);
}

TEST(portability_analyser, generic_field_access_disabled)
{
  test("struct Foo { int x;} begin { $f = (struct Foo *)0; $f->x; }", 1);
}

TEST(portability_analyser, tracepoint_field_access)
{
  test("tracepoint:sched:sched_one { args }", 0);
  test("tracepoint:sched:sched_one { args.common_field }", 0);
  test("tracepoint:sched:sched_* { args.common_field }", 0);
  // Backwards compatibility
  test("tracepoint:sched:sched_one { args->common_field }", 0);
}

class portability_analyser_btf : public test_btf {};

TEST_F(portability_analyser_btf, fentry_field_access)
{
  test("fentry:func_1 { $x = args.a; $y = args.foo1; $z = args.foo2->f.a; }",
       0);
  test("fentry:func_2 { args.foo1 }", 0);
  test("fentry:func_2, fentry:func_3 { $x = args.foo1; }", 0);
  // Backwards compatibility
  test("fentry:func_2 { args->foo1 }", 0);
}

TEST(portability_analyser, positional_params_disabled)
{
  auto bpftrace = get_mock_bpftrace();
  bpftrace->add_param("123");
  bpftrace->add_param("hello");

  test(*bpftrace, "begin { $1 }", 1);
  test(*bpftrace, "begin { str($2) }", 1);
}

TEST(portability_analyser, curtask_disabled)
{
  test("begin { curtask }", 1);
  test("struct task_struct { char comm[16]; } begin { curtask->comm }", 1);
}

TEST(portability_analyser, selective_probes_disabled)
{
  test("usdt:/bin/sh:tp1 { 1 }", 1);
  test("usdt:/bin/sh:prov1:tp1 { 1 }", 1);

  auto bpftrace = get_mock_bpftrace();
  test(*bpftrace, "watchpoint:0x10000000:8:rw { 1 }", 1);
  bpftrace->procmon_ = std::make_unique<MockProcMon>(123);
  test(*bpftrace, "watchpoint:0x10000000:8:rw { 1 }", 1);
  test(*bpftrace, "watchpoint:increment+arg1:4:w { 1 }", 1);
  test(*bpftrace, "asyncwatchpoint:increment+arg1:4:w { 1 }", 1);
}

} // namespace bpftrace::test::portability_analyser
