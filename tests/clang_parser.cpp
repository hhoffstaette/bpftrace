#include <llvm/Config/llvm-config.h>

#include "ast/attachpoint_parser.h"
#include "ast/passes/clang_parser.h"
#include "ast/passes/field_analyser.h"
#include "ast/passes/probe_expansion.h"
#include "bpftrace.h"
#include "btf_common.h"
#include "driver.h"
#include "mocks.h"
#include "struct.h"
#include "gtest/gtest.h"

namespace bpftrace::test::clang_parser {

static ast::CDefinitions parse(
    const std::string &input,
    BPFtrace &bpftrace,
    bool result = true,
    const std::string &probe = "kprobe:sys_read { 1 }")
{
  auto extended_input = input + probe;
  ast::ASTContext ast("stdin", extended_input);

  auto ok = ast::PassManager()
                .put(ast)
                .put(bpftrace)
                .add(CreateParsePass())
                .add(ast::CreateParseAttachpointsPass())
                .add(ast::CreateProbeExpansionPass())
                .add(ast::CreateFieldAnalyserPass())
                .add(ast::CreateClangParsePass())
                .run();
  EXPECT_EQ(ok && ast.diagnostics().ok(), result);
  if (ok) {
    return std::move(ok->get<ast::CDefinitions>());
  }
  return {};
}

TEST(clang_parser, integers)
{
  BPFtrace bpftrace;
  parse("struct Foo { int x; int y, z; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 12);
  ASSERT_EQ(foo->fields.size(), 3U);
  ASSERT_EQ(foo->HasField("x"), true);
  ASSERT_EQ(foo->HasField("y"), true);
  ASSERT_EQ(foo->HasField("z"), true);

  EXPECT_TRUE(foo->GetField("x").type.IsIntTy());
  EXPECT_EQ(foo->GetField("x").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("x").offset, 0);

  EXPECT_TRUE(foo->GetField("y").type.IsIntTy());
  EXPECT_EQ(foo->GetField("y").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("y").offset, 4);

  EXPECT_TRUE(foo->GetField("z").type.IsIntTy());
  EXPECT_EQ(foo->GetField("z").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("z").offset, 8);
}

TEST(clang_parser, c_union)
{
  BPFtrace bpftrace;
  parse("union Foo { char c; short s; int i; long l; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("union Foo"));
  auto foo = bpftrace.structs.Lookup("union Foo").lock();

  EXPECT_EQ(foo->size, 8);
  ASSERT_EQ(foo->fields.size(), 4U);
  ASSERT_TRUE(foo->HasField("c"));
  ASSERT_TRUE(foo->HasField("s"));
  ASSERT_TRUE(foo->HasField("i"));
  ASSERT_TRUE(foo->HasField("l"));

  EXPECT_TRUE(foo->GetField("c").type.IsIntTy());
  EXPECT_EQ(foo->GetField("c").type.GetSize(), 1U);
  EXPECT_EQ(foo->GetField("c").offset, 0);

  EXPECT_TRUE(foo->GetField("s").type.IsIntTy());
  EXPECT_EQ(foo->GetField("s").type.GetSize(), 2U);
  EXPECT_EQ(foo->GetField("s").offset, 0);

  EXPECT_TRUE(foo->GetField("i").type.IsIntTy());
  EXPECT_EQ(foo->GetField("i").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("i").offset, 0);

  EXPECT_TRUE(foo->GetField("l").type.IsIntTy());
  EXPECT_EQ(foo->GetField("l").type.GetSize(), 8U);
  EXPECT_EQ(foo->GetField("l").offset, 0);
}

TEST(clang_parser, c_enum)
{
  BPFtrace bpftrace;
  auto c_defs = parse("enum E { NONE, SOME = 99, }; struct Foo { enum E e; }",
                      bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 4);
  ASSERT_EQ(foo->fields.size(), 1U);
  ASSERT_TRUE(foo->HasField("e"));

  EXPECT_TRUE(foo->GetField("e").type.IsIntTy());
  EXPECT_EQ(foo->GetField("e").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("e").offset, 0);

  ASSERT_TRUE(c_defs.enums.contains("NONE"));
  EXPECT_EQ(std::get<0>(c_defs.enums["NONE"]), 0);
  EXPECT_EQ(std::get<1>(c_defs.enums["NONE"]), "E");
  ASSERT_TRUE(c_defs.enums.contains("SOME"));
  EXPECT_EQ(std::get<0>(c_defs.enums["SOME"]), 99);
  EXPECT_EQ(std::get<1>(c_defs.enums["SOME"]), "E");

  ASSERT_TRUE(c_defs.enum_defs.contains("E"));
  ASSERT_TRUE(c_defs.enum_defs["E"].contains(0));
  EXPECT_EQ(c_defs.enum_defs["E"][0], "NONE");
  ASSERT_TRUE(c_defs.enum_defs["E"].contains(99));
  EXPECT_EQ(c_defs.enum_defs["E"][99], "SOME");
}

TEST(clang_parser, c_enum_anonymous)
{
  BPFtrace bpftrace;
  auto c_defs = parse(
      "enum { ANON_A_VARIANT_1 = 0, ANON_A_VARIANT_2, ANON_A_CONFLICT = 99, }; "
      "enum { ANON_B_VARIANT_1 = 0, ANON_B_CONFLICT = 99, }; ",
      bpftrace);

  ASSERT_EQ(c_defs.enums.size(), 5);
  ASSERT_EQ(c_defs.enum_defs.size(), 2);

  //
  // Check enums_ contains first anonymous enum
  //

  // Check first variant present
  ASSERT_TRUE(c_defs.enums.contains("ANON_A_VARIANT_1"));
  EXPECT_EQ(std::get<0>(c_defs.enums["ANON_A_VARIANT_1"]), 0);
  auto anon_a_name = std::get<1>(c_defs.enums["ANON_A_VARIANT_1"]);
  ASSERT_FALSE(anon_a_name.empty());

  // Check second variant present
  ASSERT_TRUE(c_defs.enums.contains("ANON_A_VARIANT_2"));
  EXPECT_EQ(std::get<0>(c_defs.enums["ANON_A_VARIANT_2"]), 1);
  EXPECT_EQ(std::get<1>(c_defs.enums["ANON_A_CONFLICT"]), anon_a_name);

  // Check conflict variant present
  ASSERT_TRUE(c_defs.enums.contains("ANON_A_CONFLICT"));
  EXPECT_EQ(std::get<0>(c_defs.enums["ANON_A_CONFLICT"]), 99);
  EXPECT_EQ(std::get<1>(c_defs.enums["ANON_A_CONFLICT"]), anon_a_name);

  //
  // Check enum_defs_ contains first anonymous enum, with ANON_A_CONFLICT
  // value resolving correctly to the this enum and not the other.
  //

  ASSERT_TRUE(c_defs.enum_defs.contains(anon_a_name));
  ASSERT_EQ(c_defs.enum_defs[anon_a_name].size(), 3);
  ASSERT_TRUE(c_defs.enum_defs[anon_a_name].contains(0));
  EXPECT_EQ(c_defs.enum_defs[anon_a_name][0], "ANON_A_VARIANT_1");
  ASSERT_TRUE(c_defs.enum_defs[anon_a_name].contains(1));
  EXPECT_EQ(c_defs.enum_defs[anon_a_name][1], "ANON_A_VARIANT_2");
  ASSERT_TRUE(c_defs.enum_defs[anon_a_name].contains(99));
  EXPECT_EQ(c_defs.enum_defs[anon_a_name][99], "ANON_A_CONFLICT");

  //
  // Check enums_ contains second anonymous enum
  //

  // Check first variant present
  ASSERT_TRUE(c_defs.enums.contains("ANON_B_VARIANT_1"));
  EXPECT_EQ(std::get<0>(c_defs.enums["ANON_B_VARIANT_1"]), 0);
  auto anon_b_name = std::get<1>(c_defs.enums["ANON_B_VARIANT_1"]);
  ASSERT_FALSE(anon_b_name.empty());

  // Check conflict variant present
  ASSERT_TRUE(c_defs.enums.contains("ANON_B_CONFLICT"));
  EXPECT_EQ(std::get<0>(c_defs.enums["ANON_B_CONFLICT"]), 99);
  EXPECT_EQ(std::get<1>(c_defs.enums["ANON_B_CONFLICT"]), anon_b_name);

  //
  // Check enum_defs_ contains second anonymous enum, with ANON_B_CONFLICT
  // value resolving correctly to the this enum and not the first.
  //

  ASSERT_TRUE(c_defs.enum_defs.contains(anon_b_name));
  ASSERT_EQ(c_defs.enum_defs[anon_b_name].size(), 2);
  ASSERT_TRUE(c_defs.enum_defs[anon_b_name].contains(0));
  EXPECT_EQ(c_defs.enum_defs[anon_b_name][0], "ANON_B_VARIANT_1");
  ASSERT_TRUE(c_defs.enum_defs[anon_b_name].contains(99));
  EXPECT_EQ(c_defs.enum_defs[anon_b_name][99], "ANON_B_CONFLICT");
}

TEST(clang_parser, integer_ptr)
{
  BPFtrace bpftrace;
  parse("struct Foo { int *x; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 8);
  ASSERT_EQ(foo->fields.size(), 1U);
  ASSERT_TRUE(foo->HasField("x"));

  EXPECT_TRUE(foo->GetField("x").type.IsPtrTy());
  EXPECT_EQ(foo->GetField("x").type.GetPointeeTy()->GetIntBitWidth(),
            8 * sizeof(int));
  EXPECT_EQ(foo->GetField("x").offset, 0);
}

TEST(clang_parser, string_ptr)
{
  BPFtrace bpftrace;
  parse("struct Foo { char *str; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 8);
  ASSERT_EQ(foo->fields.size(), 1U);
  ASSERT_TRUE(foo->HasField("str"));

  const auto &ty = foo->GetField("str").type;
  const auto *pointee = ty.GetPointeeTy();
  EXPECT_TRUE(pointee->IsIntTy());
  EXPECT_EQ(pointee->GetIntBitWidth(), 8 * sizeof(char));
  EXPECT_EQ(foo->GetField("str").offset, 0);
}

TEST(clang_parser, string_array)
{
  BPFtrace bpftrace;
  parse("struct Foo { char str[32]; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 32);
  ASSERT_EQ(foo->fields.size(), 1U);
  ASSERT_TRUE(foo->HasField("str"));

  // See clang_parser.cpp; the increase signal well-formedness.
  EXPECT_TRUE(foo->GetField("str").type.IsStringTy());
  EXPECT_EQ(foo->GetField("str").type.GetSize(), 32U + 1U);
  EXPECT_EQ(foo->GetField("str").offset, 0);
}

TEST(clang_parser, nested_struct_named)
{
  BPFtrace bpftrace;
  parse("struct Bar { int x; } struct Foo { struct Bar bar; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  ASSERT_TRUE(bpftrace.structs.Has("struct Bar"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 4);
  ASSERT_EQ(foo->fields.size(), 1U);
  ASSERT_TRUE(foo->HasField("bar"));

  const auto &bar = foo->GetField("bar");
  EXPECT_TRUE(bar.type.IsRecordTy());
  EXPECT_EQ(bar.type.GetName(), "struct Bar");
  EXPECT_EQ(bar.type.GetSize(), 4U);
  EXPECT_EQ(bar.offset, 0);
}

TEST(clang_parser, nested_struct_ptr_named)
{
  BPFtrace bpftrace;
  parse("struct Bar { int x; } struct Foo { struct Bar *bar; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  ASSERT_TRUE(bpftrace.structs.Has("struct Bar"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 8);
  ASSERT_EQ(foo->fields.size(), 1U);
  ASSERT_TRUE(foo->HasField("bar"));

  const auto &bar = foo->GetField("bar");
  EXPECT_TRUE(bar.type.IsPtrTy());
  EXPECT_TRUE(bar.type.GetPointeeTy()->IsRecordTy());
  EXPECT_EQ(bar.type.GetPointeeTy()->GetName(), "struct Bar");
  EXPECT_EQ(bar.type.GetPointeeTy()->GetSize(), sizeof(int));
  EXPECT_EQ(bar.offset, 0);
}

TEST(clang_parser, nested_struct_no_type)
{
  BPFtrace bpftrace;
  // bar and baz's struct/union do not have type names, but are not anonymous
  // since they are called bar and baz
  parse("struct Foo { struct { int x; } bar; union { int y; } baz; }",
        bpftrace);

  std::string bar_name = "struct Foo::(unnamed at definitions.h:2:14)";
  std::string baz_name = "union Foo::(unnamed at definitions.h:2:37)";

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  ASSERT_TRUE(bpftrace.structs.Has(bar_name));
  ASSERT_TRUE(bpftrace.structs.Has(baz_name));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();
  auto bar = bpftrace.structs.Lookup(bar_name).lock();
  auto baz = bpftrace.structs.Lookup(baz_name).lock();

  EXPECT_EQ(foo->size, 8);
  ASSERT_EQ(foo->fields.size(), 2U);
  ASSERT_TRUE(foo->HasField("bar"));
  ASSERT_TRUE(foo->HasField("baz"));

  EXPECT_EQ(bar->size, 4);
  ASSERT_EQ(bar->fields.size(), 1U);
  ASSERT_TRUE(bar->HasField("x"));

  EXPECT_TRUE(bar->GetField("x").type.IsIntTy());
  EXPECT_EQ(bar->GetField("x").type.GetSize(), 4U);
  EXPECT_EQ(bar->GetField("x").offset, 0);

  EXPECT_EQ(baz->size, 4);
  ASSERT_EQ(baz->fields.size(), 1U);
  ASSERT_TRUE(baz->HasField("y"));

  EXPECT_TRUE(baz->GetField("y").type.IsIntTy());
  EXPECT_EQ(baz->GetField("y").type.GetSize(), 4U);
  EXPECT_EQ(baz->GetField("y").offset, 0);

  {
    const auto &bar_field = foo->GetField("bar");
    EXPECT_TRUE(bar_field.type.IsRecordTy());
    EXPECT_EQ(bar_field.type.GetSize(), sizeof(int));
    EXPECT_EQ(bar_field.offset, 0);

    const auto &baz_field = foo->GetField("baz");
    EXPECT_TRUE(baz_field.type.IsRecordTy());
    EXPECT_EQ(baz_field.type.GetSize(), sizeof(int));
    EXPECT_EQ(baz_field.offset, 4);
  }
}

TEST(clang_parser, nested_struct_unnamed_fields)
{
  BPFtrace bpftrace;
  parse("struct Foo"
        "{"
        "  struct { int x; int y; };" // Anonymous struct field
        "  int a;"
        "  struct Bar { int z; };" // Struct definition - not a field of Foo
        "}",
        bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  ASSERT_TRUE(bpftrace.structs.Has("struct Bar"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();
  auto bar = bpftrace.structs.Lookup("struct Bar").lock();

  EXPECT_EQ(foo->size, 12);
  ASSERT_EQ(foo->fields.size(), 3U);
  ASSERT_TRUE(foo->HasField("x"));
  ASSERT_TRUE(foo->HasField("y"));
  ASSERT_TRUE(foo->HasField("a"));

  EXPECT_TRUE(foo->GetField("x").type.IsIntTy());
  EXPECT_EQ(foo->GetField("x").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("x").offset, 0);
  EXPECT_TRUE(foo->GetField("y").type.IsIntTy());
  EXPECT_EQ(foo->GetField("y").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("y").offset, 4);
  EXPECT_TRUE(foo->GetField("a").type.IsIntTy());
  EXPECT_EQ(foo->GetField("a").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("a").offset, 8);

  EXPECT_EQ(bar->size, 4);
  EXPECT_EQ(bar->fields.size(), 1U);
  EXPECT_TRUE(bar->HasField("z"));

  EXPECT_TRUE(bar->GetField("z").type.IsIntTy());
  EXPECT_EQ(bar->GetField("z").type.GetSize(), 4U);
  EXPECT_EQ(bar->GetField("z").offset, 0);
}

TEST(clang_parser, nested_struct_anon_union_struct)
{
  BPFtrace bpftrace;
  parse("struct Foo"
        "{"
        "  union"
        "  {"
        "    long long _xy;"
        "    struct { int x; int y;};"
        "  };"
        "  int a;"
        "  struct { int z; };"
        "}",
        bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 16);
  ASSERT_EQ(foo->fields.size(), 5U);
  ASSERT_TRUE(foo->HasField("_xy"));
  ASSERT_TRUE(foo->HasField("x"));
  ASSERT_TRUE(foo->HasField("y"));
  ASSERT_TRUE(foo->HasField("a"));
  ASSERT_TRUE(foo->HasField("z"));

  EXPECT_TRUE(foo->GetField("_xy").type.IsIntTy());
  EXPECT_EQ(foo->GetField("_xy").type.GetSize(), 8U);
  EXPECT_EQ(foo->GetField("_xy").offset, 0);

  EXPECT_TRUE(foo->GetField("x").type.IsIntTy());
  EXPECT_EQ(foo->GetField("x").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("x").offset, 0);

  EXPECT_TRUE(foo->GetField("y").type.IsIntTy());
  EXPECT_EQ(foo->GetField("y").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("y").offset, 4);

  EXPECT_TRUE(foo->GetField("a").type.IsIntTy());
  EXPECT_EQ(foo->GetField("a").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("a").offset, 8);

  EXPECT_TRUE(foo->GetField("z").type.IsIntTy());
  EXPECT_EQ(foo->GetField("z").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("z").offset, 12);
}

TEST(clang_parser, bitfields)
{
  BPFtrace bpftrace;
  parse("struct Foo { int a:8, b:8, c:16; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 4);
  ASSERT_EQ(foo->fields.size(), 3U);
  ASSERT_TRUE(foo->HasField("a"));
  ASSERT_TRUE(foo->HasField("b"));
  ASSERT_TRUE(foo->HasField("c"));

  // clang-tidy doesn't seem to acknowledge that ASSERT_*() will
  // return from function so that these are in fact checked accesses.
  //
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_TRUE(foo->GetField("a").type.IsIntTy());
  EXPECT_EQ(foo->GetField("a").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("a").offset, 0);
  ASSERT_TRUE(foo->GetField("a").bitfield.has_value());
  EXPECT_EQ(foo->GetField("a").bitfield->read_bytes, 0x1U);
  EXPECT_EQ(foo->GetField("a").bitfield->access_rshift, 0U);
  EXPECT_EQ(foo->GetField("a").bitfield->mask, 0xFFU);

  EXPECT_TRUE(foo->GetField("b").type.IsIntTy());
  EXPECT_EQ(foo->GetField("b").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("b").offset, 1);
  EXPECT_TRUE(foo->GetField("b").bitfield.has_value());
  EXPECT_EQ(foo->GetField("b").bitfield->read_bytes, 0x1U);
  EXPECT_EQ(foo->GetField("b").bitfield->access_rshift, 0U);
  EXPECT_EQ(foo->GetField("b").bitfield->mask, 0xFFU);

  EXPECT_TRUE(foo->GetField("c").type.IsIntTy());
  EXPECT_EQ(foo->GetField("c").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("c").offset, 2);
  EXPECT_TRUE(foo->GetField("c").bitfield.has_value());
  EXPECT_EQ(foo->GetField("c").bitfield->read_bytes, 0x2U);
  EXPECT_EQ(foo->GetField("c").bitfield->access_rshift, 0U);
  EXPECT_EQ(foo->GetField("c").bitfield->mask, 0xFFFFU);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(clang_parser, bitfields_uneven_fields)
{
  BPFtrace bpftrace;
  parse("struct Foo { int a:1, b:1, c:3, d:20, e:7; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 4);
  ASSERT_EQ(foo->fields.size(), 5U);
  ASSERT_TRUE(foo->HasField("a"));
  ASSERT_TRUE(foo->HasField("b"));
  ASSERT_TRUE(foo->HasField("c"));
  ASSERT_TRUE(foo->HasField("d"));
  ASSERT_TRUE(foo->HasField("e"));

  // clang-tidy doesn't seem to acknowledge that ASSERT_*() will
  // return from function so that these are in fact checked accesses.
  //
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_TRUE(foo->GetField("a").type.IsIntTy());
  EXPECT_EQ(foo->GetField("a").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("a").offset, 0);
  ASSERT_TRUE(foo->GetField("a").bitfield.has_value());
  EXPECT_EQ(foo->GetField("a").bitfield->read_bytes, 1U);
  EXPECT_EQ(foo->GetField("a").bitfield->access_rshift, 0U);
  EXPECT_EQ(foo->GetField("a").bitfield->mask, 0x1U);

  EXPECT_TRUE(foo->GetField("b").type.IsIntTy());
  EXPECT_EQ(foo->GetField("b").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("b").offset, 0);
  ASSERT_TRUE(foo->GetField("b").bitfield.has_value());
  EXPECT_EQ(foo->GetField("b").bitfield->read_bytes, 1U);
  EXPECT_EQ(foo->GetField("b").bitfield->access_rshift, 1U);
  EXPECT_EQ(foo->GetField("b").bitfield->mask, 0x1U);

  EXPECT_TRUE(foo->GetField("c").type.IsIntTy());
  EXPECT_EQ(foo->GetField("c").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("c").offset, 0);
  ASSERT_TRUE(foo->GetField("c").bitfield.has_value());
  EXPECT_EQ(foo->GetField("c").bitfield->read_bytes, 1U);
  EXPECT_EQ(foo->GetField("c").bitfield->access_rshift, 2U);
  EXPECT_EQ(foo->GetField("c").bitfield->mask, 0x7U);

  EXPECT_TRUE(foo->GetField("d").type.IsIntTy());
  EXPECT_EQ(foo->GetField("d").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("d").offset, 0);
  ASSERT_TRUE(foo->GetField("d").bitfield.has_value());
  EXPECT_EQ(foo->GetField("d").bitfield->read_bytes, 4U);
  EXPECT_EQ(foo->GetField("d").bitfield->access_rshift, 5U);
  EXPECT_EQ(foo->GetField("d").bitfield->mask, 0xFFFFFU);

  EXPECT_TRUE(foo->GetField("e").type.IsIntTy());
  EXPECT_EQ(foo->GetField("e").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("e").offset, 3);
  ASSERT_TRUE(foo->GetField("e").bitfield.has_value());
  EXPECT_EQ(foo->GetField("e").bitfield->read_bytes, 1U);
  EXPECT_EQ(foo->GetField("e").bitfield->access_rshift, 1U);
  EXPECT_EQ(foo->GetField("e").bitfield->mask, 0x7FU);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(clang_parser, bitfields_with_padding)
{
  BPFtrace bpftrace;
  parse("struct Foo { int pad; int a:28, b:4; long int end;}", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  // clang-tidy doesn't seem to acknowledge that ASSERT_*() will
  // return from function so that these are in fact checked accesses.
  //
  // NOLINTBEGIN(bugprone-unchecked-optional-access)
  EXPECT_EQ(foo->size, 16);
  ASSERT_EQ(foo->fields.size(), 4U);
  ASSERT_TRUE(foo->HasField("pad"));
  ASSERT_TRUE(foo->HasField("a"));
  ASSERT_TRUE(foo->HasField("b"));
  ASSERT_TRUE(foo->HasField("end"));

  EXPECT_TRUE(foo->GetField("a").type.IsIntTy());
  EXPECT_EQ(foo->GetField("a").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("a").offset, 4);
  ASSERT_TRUE(foo->GetField("a").bitfield.has_value());
  EXPECT_EQ(foo->GetField("a").bitfield->read_bytes, 4U);
  EXPECT_EQ(foo->GetField("a").bitfield->access_rshift, 0U);
  EXPECT_EQ(foo->GetField("a").bitfield->mask, 0xFFFFFFFU);

  EXPECT_TRUE(foo->GetField("b").type.IsIntTy());
  EXPECT_EQ(foo->GetField("b").type.GetSize(), 4U);
  EXPECT_EQ(foo->GetField("b").offset, 7);
  ASSERT_TRUE(foo->GetField("b").bitfield.has_value());
  EXPECT_EQ(foo->GetField("b").bitfield->read_bytes, 1U);
  EXPECT_EQ(foo->GetField("b").bitfield->access_rshift, 4U);
  EXPECT_EQ(foo->GetField("b").bitfield->mask, 0xFU);
  // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(clang_parser, builtin_headers)
{
  // size_t is defined in stddef.h
  BPFtrace bpftrace;
  parse("#include <stddef.h>\nstruct Foo { size_t x, y, z; }", bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct Foo"));
  auto foo = bpftrace.structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 24);
  ASSERT_EQ(foo->fields.size(), 3U);
  ASSERT_TRUE(foo->HasField("x"));
  ASSERT_TRUE(foo->HasField("y"));
  ASSERT_TRUE(foo->HasField("z"));

  EXPECT_TRUE(foo->GetField("x").type.IsIntTy());
  EXPECT_EQ(foo->GetField("x").type.GetSize(), 8U);
  EXPECT_EQ(foo->GetField("x").offset, 0);

  EXPECT_TRUE(foo->GetField("y").type.IsIntTy());
  EXPECT_EQ(foo->GetField("y").type.GetSize(), 8U);
  EXPECT_EQ(foo->GetField("y").offset, 8);

  EXPECT_TRUE(foo->GetField("z").type.IsIntTy());
  EXPECT_EQ(foo->GetField("z").type.GetSize(), 8U);
  EXPECT_EQ(foo->GetField("z").offset, 16);
}

TEST(clang_parser, macro_preprocessor)
{
  BPFtrace bpftrace;

  auto c_defs = parse("#define FOO size_t\n k:f { 0 }", bpftrace);
  ASSERT_EQ(c_defs.macros.count("FOO"), 1U);
  EXPECT_EQ(c_defs.macros["FOO"], "size_t");

  c_defs = parse("#define _UNDERSCORE 314\n k:f { 0 }", bpftrace);
  ASSERT_EQ(c_defs.macros.count("_UNDERSCORE"), 1U);
  EXPECT_EQ(c_defs.macros["_UNDERSCORE"], "314");
}

TEST(clang_parser, parse_fail)
{
  BPFtrace bpftrace;
  parse("struct a { int a; struct b b; };", bpftrace, false);
}

class clang_parser_btf : public test_btf {};

TEST_F(clang_parser_btf, btf)
{
  auto bpftrace = get_mock_bpftrace();
  parse("struct Foo { "
        "  struct Foo1 f1;"
        "  struct Foo2 f2;"
        "  struct Foo3 f3;"
        "  struct Foo4 f4;"
        "}",
        *bpftrace);

  ASSERT_TRUE(bpftrace->structs.Has("struct Foo1"));
  ASSERT_TRUE(bpftrace->structs.Has("struct Foo2"));
  ASSERT_TRUE(bpftrace->structs.Has("struct Foo3"));
  ASSERT_TRUE(bpftrace->structs.Has("struct Foo4"));
  auto foo1 = bpftrace->structs.Lookup("struct Foo1").lock();
  auto foo2 = bpftrace->structs.Lookup("struct Foo2").lock();
  auto foo3 = bpftrace->structs.Lookup("struct Foo3").lock();
  auto foo4 = bpftrace->structs.Lookup("struct Foo4").lock();

  EXPECT_EQ(foo1->size, 16);
  ASSERT_EQ(foo1->fields.size(), 3U);
  ASSERT_TRUE(foo1->HasField("a"));
  ASSERT_TRUE(foo1->HasField("b"));
  ASSERT_TRUE(foo1->HasField("c"));

  EXPECT_TRUE(foo1->GetField("a").type.IsIntTy());
  EXPECT_EQ(foo1->GetField("a").type.GetSize(), 4U);
  EXPECT_EQ(foo1->GetField("a").offset, 0);

  EXPECT_TRUE(foo1->GetField("b").type.IsIntTy());
  EXPECT_EQ(foo1->GetField("b").type.GetSize(), 1U);
  EXPECT_EQ(foo1->GetField("b").offset, 4);

  EXPECT_TRUE(foo1->GetField("c").type.IsIntTy());
  EXPECT_EQ(foo1->GetField("c").type.GetSize(), 8U);
  EXPECT_EQ(foo1->GetField("c").offset, 8);

  EXPECT_EQ(foo2->size, 24);
  ASSERT_EQ(foo2->fields.size(), 3U);
  ASSERT_TRUE(foo2->HasField("a"));
  ASSERT_TRUE(foo2->HasField("f"));
  ASSERT_TRUE(foo2->HasField("g"));

  EXPECT_TRUE(foo2->GetField("a").type.IsIntTy());
  EXPECT_EQ(foo2->GetField("a").type.GetSize(), 4U);
  EXPECT_EQ(foo2->GetField("a").offset, 0);

  EXPECT_TRUE(foo2->GetField("f").type.IsRecordTy());
  EXPECT_EQ(foo2->GetField("f").type.GetSize(), 16U);
  EXPECT_EQ(foo2->GetField("f").offset, 8);

  EXPECT_TRUE(foo2->GetField("g").type.IsIntTy());
  EXPECT_EQ(foo2->GetField("g").type.GetSize(), 1U);
  EXPECT_EQ(foo2->GetField("g").offset, 8);

  EXPECT_EQ(foo3->size, 16);
  ASSERT_EQ(foo3->fields.size(), 2U);
  ASSERT_TRUE(foo3->HasField("foo1"));
  ASSERT_TRUE(foo3->HasField("foo2"));

  EXPECT_EQ(foo4->size, 16);
  ASSERT_EQ(foo4->fields.size(), 7U);
  ASSERT_TRUE(foo4->HasField("pid"));
  ASSERT_TRUE(foo4->HasField("pgid"));
  ASSERT_TRUE(foo4->HasField("a"));
  ASSERT_TRUE(foo4->HasField("b"));
  ASSERT_TRUE(foo4->HasField("c"));
  ASSERT_TRUE(foo4->HasField("d"));

  auto foo1_field = foo3->GetField("foo1");
  auto foo2_field = foo3->GetField("foo2");
  EXPECT_TRUE(foo1_field.type.IsPtrTy());
  EXPECT_EQ(foo1_field.type.GetPointeeTy()->GetName(), "struct Foo1");
  EXPECT_EQ(foo1_field.offset, 0);

  EXPECT_TRUE(foo2_field.type.IsPtrTy());
  EXPECT_EQ(foo2_field.type.GetPointeeTy()->GetName(), "struct Foo2");
  EXPECT_EQ(foo2_field.offset, 8);
}

// Disabled because BTF flattens multi-dimensional arrays #3082.
TEST_F(clang_parser_btf, DISABLED_btf_arrays_multi_dim)
{
  auto bpftrace = get_mock_bpftrace();
  parse("struct Foo { struct Arrays a; };", *bpftrace);

  ASSERT_TRUE(bpftrace->structs.Has("struct Arrays"));
  auto arrs = bpftrace->structs.Lookup("struct Arrays").lock();

  ASSERT_TRUE(arrs->HasField("multi_dim"));
  EXPECT_TRUE(arrs->GetField("multi_dim").type.IsArrayTy());
  EXPECT_EQ(arrs->GetField("multi_dim").offset, 40);
  EXPECT_EQ(arrs->GetField("multi_dim").type.GetSize(), 24U);
  EXPECT_EQ(arrs->GetField("multi_dim").type.GetNumElements(), 3);

  EXPECT_TRUE(arrs->GetField("multi_dim").type.GetElementTy()->IsArrayTy());
  EXPECT_EQ(arrs->GetField("multi_dim").type.GetElementTy()->GetSize(), 8U);
  EXPECT_EQ(arrs->GetField("multi_dim").type.GetElementTy()->GetNumElements(),
            2);

  EXPECT_TRUE(arrs->GetField("multi_dim")
                  .type.GetElementTy()
                  ->GetElementTy()
                  ->IsIntTy());
  EXPECT_EQ(arrs->GetField("multi_dim")
                .type.GetElementTy()
                ->GetElementTy()
                ->GetSize(),
            4U);
}

TEST(clang_parser, btf_unresolved_typedef)
{
  // size_t is defined in stddef.h, but if we have BTF, it should be possible to
  // extract it from there
  auto bpftrace = get_mock_bpftrace();
  if (!bpftrace->has_btf_data())
    GTEST_SKIP();

  parse("struct Foo { size_t x; };", *bpftrace);

  ASSERT_TRUE(bpftrace->structs.Has("struct Foo"));
  auto foo = bpftrace->structs.Lookup("struct Foo").lock();

  EXPECT_EQ(foo->size, 8);
  ASSERT_EQ(foo->fields.size(), 1U);
  ASSERT_TRUE(foo->HasField("x"));

  EXPECT_TRUE(foo->GetField("x").type.IsIntTy());
  EXPECT_EQ(foo->GetField("x").type.GetSize(), 8U);
  EXPECT_EQ(foo->GetField("x").offset, 0);
}

TEST_F(clang_parser_btf, btf_type_override)
{
  // It should be possible to override types from BTF, ...
  auto bpftrace = get_mock_bpftrace();
  parse("struct Foo1 { int a; };\n",
        *bpftrace,
        true,
        "kprobe:sys_read { @x = ((struct Foo1 *)curtask); }");

  ASSERT_TRUE(bpftrace->structs.Has("struct Foo1"));
  auto foo1 = bpftrace->structs.Lookup("struct Foo1").lock();
  ASSERT_EQ(foo1->fields.size(), 1U);
  ASSERT_TRUE(foo1->HasField("a"));

  // ... however, in such case, no other types are taken from BTF and the
  // following will fail since Foo2 will be undefined
  bpftrace->btf_set_.clear();
  parse("struct Foo1 { struct Foo2 foo2; };\n",
        *bpftrace,
        false,
        "kprobe:sys_read { @x = ((struct Foo1 *)curtask); }");

  // Here, Foo1 redefinition will take place when resolving incomplete types
  // (since Foo3 contains a pointer to Foo1)
  bpftrace->btf_set_.clear();
  parse("struct Foo1 { struct Foo2 foo2; };\n",
        *bpftrace,
        false,
        "kprobe:sys_read { @x1 = ((struct Foo3 *)curtask); }");
}

TEST(clang_parser, struct_typedef)
{
  // Make sure we can differentiate between "struct max_align_t {}" and
  // "typedef struct {} max_align_t"
  BPFtrace bpftrace;
  parse("#include <__stddef_max_align_t.h>\n"
        "struct max_align_t { int x; };",
        bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct max_align_t"));
  ASSERT_TRUE(bpftrace.structs.Has("max_align_t"));
  auto max_align_struct = bpftrace.structs.Lookup("struct max_align_t").lock();
  auto max_align_typedef = bpftrace.structs.Lookup("max_align_t").lock();

  // Non-typedef'd struct
  EXPECT_EQ(max_align_struct->size, 4);
  ASSERT_EQ(max_align_struct->fields.size(), 1U);
  ASSERT_TRUE(max_align_struct->HasField("x"));

  EXPECT_TRUE(max_align_struct->GetField("x").type.IsIntTy());
  EXPECT_EQ(max_align_struct->GetField("x").type.GetSize(), 4U);
  EXPECT_EQ(max_align_struct->GetField("x").offset, 0);

  // typedef'd struct (defined in __stddef_max_align_t.h builtin header)
  EXPECT_EQ(max_align_typedef->size, 32);
  ASSERT_EQ(max_align_typedef->fields.size(), 2U);
  ASSERT_TRUE(max_align_typedef->HasField("__clang_max_align_nonce1"));
  ASSERT_TRUE(max_align_typedef->HasField("__clang_max_align_nonce2"));

  EXPECT_TRUE(
      max_align_typedef->GetField("__clang_max_align_nonce1").type.IsIntTy());
  EXPECT_EQ(
      max_align_typedef->GetField("__clang_max_align_nonce1").type.GetSize(),
      8U);
  EXPECT_EQ(max_align_typedef->GetField("__clang_max_align_nonce1").offset, 0);

  // double are not parsed correctly yet so these fields are junk for now
  EXPECT_TRUE(
      max_align_typedef->GetField("__clang_max_align_nonce2").type.IsNoneTy());
  EXPECT_EQ(
      max_align_typedef->GetField("__clang_max_align_nonce2").type.GetSize(),
      0U);
  EXPECT_EQ(max_align_typedef->GetField("__clang_max_align_nonce2").offset, 16);
}

TEST(clang_parser, struct_qualifiers)
{
  BPFtrace bpftrace;
  parse("struct a {int a} struct b { volatile const struct a* restrict a; "
        "const struct a a2; };",
        bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct b"));
  auto SB = bpftrace.structs.Lookup("struct b").lock();
  EXPECT_EQ(SB->size, 16);
  EXPECT_EQ(SB->fields.size(), 2U);

  EXPECT_TRUE(SB->GetField("a").type.IsPtrTy());
  EXPECT_TRUE(SB->GetField("a").type.GetPointeeTy()->IsRecordTy());
  EXPECT_EQ(SB->GetField("a").type.GetPointeeTy()->GetName(), "struct a");

  EXPECT_TRUE(SB->GetField("a2").type.IsRecordTy());
  EXPECT_EQ(SB->GetField("a2").type.GetName(), "struct a");
}

TEST(clang_parser, redefined_types)
{
  BPFtrace bpftrace;
  parse("struct a {int a;}; struct a {int a;};", bpftrace, false);
  parse("struct a {int a;}; struct a {int a; short b;};", bpftrace, false);
}

TEST(clang_parser, data_loc_annotation)
{
  BPFtrace bpftrace;
  std::string input = R"_(
struct _tracepoint_irq_irq_handler_entry
{
  int common_pid;
  int irq;
  __attribute__((annotate("tp_data_loc"))) char * name;
};
  )_";
  parse(input, bpftrace);

  ASSERT_TRUE(bpftrace.structs.Has("struct _tracepoint_irq_irq_handler_entry"));

  auto s = bpftrace.structs.Lookup("struct _tracepoint_irq_irq_handler_entry")
               .lock();
  EXPECT_EQ(s->size, 16);
  EXPECT_EQ(s->fields.size(), 3U);

  EXPECT_TRUE(s->GetField("common_pid").type.IsIntTy());
  EXPECT_TRUE(s->GetField("irq").type.IsIntTy());

  // The parser needs to rewrite __data_loc fields to be u64 so it can hold
  // a pointer to the actual data. The kernel tracepoint infra exports an
  // encoded u32 which codegen will know how to decode.
  EXPECT_TRUE(s->GetField("name").is_data_loc);
  ASSERT_TRUE(s->GetField("name").type.IsIntTy());
  EXPECT_EQ(s->GetField("name").type.GetIntBitWidth(), 64ULL);
}

} // namespace bpftrace::test::clang_parser
