#include "common.h"

namespace bpftrace::test::codegen {

TEST(codegen, map_assign_array)
{
  test("struct Foo { int arr[4]; }"
       "kprobe:f"
       "{"
       "  @x[0] = ((struct Foo *)arg0)->arr;"
       "  $var = @x[0][0]; "
       "}",

       NAME);
}

} // namespace bpftrace::test::codegen
