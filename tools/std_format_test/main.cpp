// Compiles and runs runtime/platform/android/std_format.h without Android headers.
//
// The Android build force-includes the shim (runtime/platform/compat.h) because the NDK's libc++
// has no std::format at minSdk 26, so its behaviour is the logging's behaviour. Every assertion
// below is a format string taken from the runtime, with the argument types it is given there.
#define __ANDROID__ 1
#include <cstdio>
#include <string>
#include <vector>
#include "platform/android/std_format.h"

static int failures = 0;
static void check(const std::string& got, const std::string& want, const char* what)
{
    if (got != want) { std::printf("FAIL %s: got [%s] want [%s]\n", what, got.c_str(), want.c_str()); failures++; }
}

int main()
{
    // The specifiers that actually appear in the runtime, with the argument types they are given.
    check(std::format("{:08x}", 0x1234u), "00001234", "08x unsigned");
    check(std::format("{:08x}", 0x1234), "00001234", "08x int");
    check(std::format("{:06x}", 0xabc), "000abc", "06x");
    check(std::format("{:02x}", 255), "ff", "02x");
    check(std::format("{:X}", 0xbeef), "BEEF", "X");
    check(std::format("{:08X}", 0xbeef), "0000BEEF", "08X");
    check(std::format("{:04x}", 5), "0005", "04x");
    check(std::format("{:016x}", 1ull), "0000000000000001", "016x");
    check(std::format("{:#06x}", 0xab), "0x00ab", "#06x");
    check(std::format("{:+#x}", 0xab), "+0xab", "+#x");
    check(std::format("{:x}", 4095), "fff", "x");
    check(std::format("{:02d}", 7), "07", "02d");
    check(std::format("{:04d}", 42), "0042", "04d");
    check(std::format("{:02}", 9), "09", "02 default");
    check(std::format("{:2}", 9), " 9", "width only");
    check(std::format("{:<2}", 9), "9 ", "<2");
    check(std::format("{:4}", 9), "   9", "4");
    check(std::format("{:04}", 9), "0009", "04");
    check(std::format("{:.2f}", 3.14159), "3.14", ".2f");
    check(std::format("{:.1f}", 2.05), "2.0", ".1f (2.05 is not exact)");
    check(std::format("{:.3f}", 1.0), "1.000", ".3f");
    check(std::format("{:.4f}", 0.5), "0.5000", ".4f");
    check(std::format("{:.0f}", 2.6), "3", ".0f");
    check(std::format("{:.6f}", 1.0 / 3), "0.333333", ".6f");
    check(std::format("{:5.1f}", 2.0), "  2.0", "5.1f");
    check(std::format("{:.6}", 0.1), "0.1", ".6 (general)");
    check(std::format("{:.2f}", -0.5), "-0.50", "negative float");
    check(std::format("{:08x}", -1), "ffffffff", "negative as hex (two's complement)");
    check(std::format("{}", -12), "-12", "plain negative");
    check(std::format("{}", std::string("hi")), "hi", "string");
    check(std::format("{}", "lit"), "lit", "literal");
    check(std::format("{}", true), "true", "bool");
    check(std::format("{}", 3ull), "3", "unsigned long long");
    check(std::format("{{{}}}", 1), "{1}", "escaped braces");
    check(std::format("{} and {}", 1, 2), "1 and 2", "two automatic");
    check(std::format("{1}-{0}", 1, 2), "2-1", "positional");
    check(std::format("{0}-{0}", 5), "5-5", "repeated positional");
    check(std::format("{:08x} {}", 0x10, "name"), "00000010 name", "mixed");
    check(std::format("{:.2f}%%", 50.0), "50.00%%", "a percent is literal, not an escape");
    check(std::format("{}", 255ull), "255", "{} on ull is decimal");
    check(std::format("{:08x}", static_cast<unsigned char>(255)), "000000ff", "uchar");
    check(std::format("{:d}", 12), "12", "d");
    check(std::format("{:c}", 65), "A", "c");
    check(std::format("{:o}", 8), "10", "o");
    check(std::format("{:b}", 5), "101", "b");
    check(std::format("{:e}", 1234.5), "1.234500e+03", "e default precision");

    // vformat, used by shadergen.cpp with a run-time format string.
    check(std::vformat("{}", std::make_format_args("alphaRef.x")), "alphaRef.x", "vformat string");
    const std::string spec_holder = "1.0";
    check(std::vformat("{:.1f}", std::make_format_args(1.0)), "1.0", "vformat float");

    // Out-of-range indexes and unknown specs must not read past the argument list.
    check(std::format("{2}", 1), "<missing argument>", "out of range");
    std::printf(failures ? "%d FAILURES\n" : "all format checks passed\n", failures);
    return failures ? 1 : 0;
}
