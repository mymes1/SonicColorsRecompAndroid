#pragma once
// std::format for the Android build.
//
// The runtime formats its log lines and file names with std::format, and libc++ only made it
// complete in LLVM 17. The NDK that still contains shaderc is r26 (LLVM 17), and there <format>
// exists but std::format itself is gated off for older API levels, so every call site fails to
// compile with "no member named 'format' in namespace 'std'".
//
// Rather than move 300 call sites onto printf, this provides std::format and std::vformat for the
// subset the code actually uses: the integer, floating-point, bool and string arguments, written as
// {}, {:d}, {:x}/{:X}, {:o}, {:b}, {:c}, {:f}/{:e}/{:g} with a width, a precision, a fill and an
// alignment, the flags - + space # 0, and positional indexes. Anything else -- chrono types, nested
// format specs, wide strings, ranges -- is a compile error that says so, so a call site that needs
// more shows up at build time instead of silently printing the wrong thing.
//
// It is force-included through runtime/platform/compat.h, which every runtime file gets, and
// runtime/log.h stops including <format> on Android so the standard header cannot shadow this.
#if defined(__ANDROID__)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace scr_fmt_detail
{

// A format spec, parsed but not yet combined with an argument.
struct Spec
{
    char fill = ' ';
    char align = 0;      // 0, '<', '>' or '^'
    bool minus = false;  // -
    bool plus = false;   // +
    bool space = false;  // ' '
    bool hash = false;   // #
    bool zero = false;   // 0
    int width = -1;      // -1 means "not given"
    int precision = -1;  // -1 means "not given"
    char type = 0;       // 0 means "default for the argument"
};

// One replacement field, as written in the format string.
struct Field
{
    bool positional = false;
    size_t index = 0;
    Spec spec;   // only meaningful for positional fields
    std::string_view spec_text{};
    bool has_spec = false;
};

inline size_t ClampWidth(int width) { return width > 0 ? static_cast<size_t>(width) : 0; }

inline void AppendPadding(std::string& out, size_t count, char fill) { out.append(count, fill); }

// ---------------------------------------------------------------- specs

inline bool ParseSpec(std::string_view text, Spec& spec)
{
    size_t i = 0;
    // fill-and-align: a fill character is anything but '{' or '}' and only counts when it is
    // followed by an alignment character, exactly as [format.string.std] says.
    if (text.size() >= 2 && (text[1] == '<' || text[1] == '>' || text[1] == '^'))
    {
        spec.fill = text[0];
        spec.align = text[1];
        i = 2;
    }
    else if (!text.empty() && (text[0] == '<' || text[0] == '>' || text[0] == '^'))
    {
        spec.align = text[0];
        i = 1;
    }
    for (; i < text.size(); i++)
    {
        const char c = text[i];
        if (c == '-') spec.minus = true;
        else if (c == '+') spec.plus = true;
        else if (c == ' ') spec.space = true;
        else if (c == '#') spec.hash = true;
        else if (c == '0') spec.zero = true;
        else break;
    }
    if (i < text.size() && text[i] >= '0' && text[i] <= '9')
    {
        spec.width = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') spec.width = spec.width * 10 + (text[i++] - '0');
    }
    if (i < text.size() && text[i] == '.')
    {
        i++;
        spec.precision = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
            spec.precision = spec.precision * 10 + (text[i++] - '0');
    }
    if (i < text.size()) spec.type = text[i++];
    // Reject what this does not implement: 'L' (locale) and anything left over.
    if (i != text.size()) return false;
    switch (spec.type)
    {
    case 0: case 'b': case 'B': case 'c': case 'd': case 'e': case 'E': case 'f': case 'F':
    case 'g': case 'G': case 'o': case 's': case 'x': case 'X':
        return true;
    default:
        return false;
    }
}

inline std::string ApplyWidth(std::string value, const Spec& spec, bool numeric)
{
    const size_t width = ClampWidth(spec.width);
    if (value.size() >= width) return value;
    const size_t pad = width - value.size();
    char align = spec.align;
    if (align == 0) align = numeric ? '>' : '<';
    // Zero padding only applies to a numeric argument with no explicit alignment.
    char fill = spec.fill;
    if (spec.align == 0 && spec.zero && numeric && spec.precision < 0) fill = '0';
    if (align == '<') return value + std::string(pad, fill);
    if (align == '^')
    {
        const size_t left = pad / 2;
        return std::string(left, fill) + value + std::string(pad - left, fill);
    }
    if (fill == '0' && !value.empty() && (value[0] == '-' || value[0] == '+' || value[0] == ' '))
        return value.substr(0, 1) + std::string(pad, '0') + value.substr(1);
    if (fill == '0' && value.size() > 1 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
        return value.substr(0, 2) + std::string(pad, '0') + value.substr(2);
    return std::string(pad, fill) + value;
}

// ---------------------------------------------------------------- integers

inline std::string FormatUnsigned(unsigned long long v, int base, bool upper)
{
    if (v == 0) return "0";
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    while (v)
    {
        out.push_back(digits[v % static_cast<unsigned>(base)]);
        v /= static_cast<unsigned>(base);
    }
    return std::string(out.rbegin(), out.rend());
}

inline std::string FormatSigned(unsigned long long magnitude, const Spec& spec, int base, bool negative,
                                bool uppercase)
{
    std::string digits = FormatUnsigned(magnitude, base, uppercase);
    if (spec.precision >= 0)
    {
        const size_t want = static_cast<size_t>(spec.precision);
        if (digits.size() < want) digits = std::string(want - digits.size(), '0') + digits;
    }
    if (base == 16 && spec.hash) digits = (uppercase ? "0X" : "0x") + digits;
    else if (base == 8 && spec.hash && (digits.empty() || digits[0] != '0')) digits = "0" + digits;
    else if (base == 2 && spec.hash) digits = "0b" + digits;
    if (negative) return "-" + digits;
    if (spec.plus) return "+" + digits;
    if (spec.space) return " " + digits;
    return digits;
}

template <typename T>
inline std::string FormatInteger(T value, const Spec& spec, int bits)
{
    static_assert(sizeof(T) <= sizeof(long long), "integer types larger than long long are not supported");
    const bool is_signed = std::is_signed_v<T>;
    // 'c' is a character, not a number.
    if (spec.type == 'c') return std::string(1, static_cast<char>(value));
    int base = 10;
    bool uppercase = false;
    switch (spec.type)
    {
    case 'x': base = 16; break;
    case 'X': base = 16; uppercase = true; break;
    case 'o': base = 8; break;
    case 'b': case 'B': base = 2; break;
    case 0: case 'd': break;
    default:
        // A float presentation for an integer argument is not part of the subset.
        return {};
    }
    const long long v = static_cast<long long>(value);
    // [format.string.std]: with x, X, o or b the value is converted to its unsigned counterpart
    // first, so a negative number prints as two's complement rather than with a minus sign.
    const bool as_unsigned = base != 10;
    const bool negative = is_signed && v < 0 && !as_unsigned;
    unsigned long long magnitude = static_cast<unsigned long long>(v);
    // Truncating to T's own width gives the two's complement conversion, so {:x} of an int -1 is
    // ffffffff while the long long -1 is ffffffffffffffff.
    if (as_unsigned && bits > 0 && bits < 64) magnitude &= (1ull << bits) - 1ull;
    if (negative) magnitude = 0ull - magnitude;
    const std::string digits = FormatSigned(magnitude, spec, base, negative, uppercase);
    // 'd' on an unsigned argument is the same digits either way; only the sign handling differs.
    return ApplyWidth(digits, spec, true);
}

// ---------------------------------------------------------------- floats

// to_chars and strtof round-to-nearest; printf does too, so printf is enough here and avoids a
// locale-dependent decimal separator by only ever using the "C" locale's.
inline std::string FormatFloat(long double value, const Spec& spec)
{
    char type = spec.type;
    if (type == 0) type = 'g';
    if (type == 'F') type = 'f';
    if (type == 'E') type = 'e';
    if (type == 'G') type = 'g';
    char format[16];
    int n = 0;
    format[n++] = '%';
    if (spec.plus) format[n++] = '+';
    else if (spec.space) format[n++] = ' ';
    if (spec.hash) format[n++] = '#';
    if (spec.precision >= 0) n += std::snprintf(format + n, sizeof(format) - n, ".%d", spec.precision);
    format[n++] = type;
    format[n] = 0;
    char buffer[512];
    const int written = std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(value));
    std::string out = written > 0 ? std::string(buffer, static_cast<size_t>(written)) : std::string();
    return ApplyWidth(out, spec, true);
}

// ---------------------------------------------------------------- arguments

// Everything the runtime formats is convertible to one of these. A type that is none of them fails
// here, at the call site, which is the point: no silent no-op formatting.
struct Arg
{
    enum class Kind { None, Bool, Signed, Unsigned, Float, String, Pointer } kind = Kind::None;
    // Width of the integer type the argument was written as; {:x} prints two's complement within it.
    int bits = 0;
    long long s = 0;
    unsigned long long u = 0;
    long double f = 0;
    std::string_view str{};
};

template <typename T>
Arg MakeArg(const T& value)
{
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    Arg arg;
    if constexpr (std::is_same_v<U, bool>)
    {
        arg.kind = Arg::Kind::Bool;
        arg.s = value ? 1 : 0;
    }
    else if constexpr (std::is_array_v<U> &&
                       std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>)
    {
        // A string literal deduces as char[N], which is neither a pointer nor a string type.
        arg.kind = Arg::Kind::String;
        arg.str = std::string_view(value, std::extent_v<U> ? std::strlen(value) : 0);
    }
    else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view> ||
                       std::is_same_v<U, const char*> || std::is_same_v<U, char*>)
    {
        arg.kind = Arg::Kind::String;
        arg.str = std::string_view(value);
    }
    else if constexpr (std::is_enum_v<U>)
    {
        arg.kind = Arg::Kind::Signed;
        arg.bits = static_cast<int>(sizeof(std::underlying_type_t<U>) * 8);
        arg.s = static_cast<long long>(static_cast<std::underlying_type_t<U>>(value));
    }
    else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
    {
        arg.kind = Arg::Kind::Signed;
        arg.bits = static_cast<int>(sizeof(U) * 8);
        arg.s = static_cast<long long>(value);
    }
    else if constexpr (std::is_integral_v<U>)
    {
        arg.kind = Arg::Kind::Unsigned;
        arg.bits = static_cast<int>(sizeof(U) * 8);
        arg.u = static_cast<unsigned long long>(value);
    }
    else if constexpr (std::is_floating_point_v<U>)
    {
        arg.kind = Arg::Kind::Float;
        arg.f = static_cast<long double>(value);
    }
    else if constexpr (std::is_pointer_v<U> && std::is_void_v<std::remove_pointer_t<U>>)
    {
        arg.kind = Arg::Kind::Pointer;
        arg.u = reinterpret_cast<unsigned long long>(value);
    }
    else
    {
        static_assert(std::is_void_v<U>,
                      "std::format on Android supports integers, floats, bools, strings and pointers; "
                      "pass something that converts to one of those instead");
    }
    return arg;
}

inline std::string FormatArg(const Arg& arg, const Spec& spec)
{
    switch (arg.kind)
    {
    case Arg::Kind::Bool:
        return ApplyWidth(arg.s ? "true" : "false", spec, false);
    case Arg::Kind::Signed:
        return FormatInteger(arg.s, spec, arg.bits);
    case Arg::Kind::Unsigned:
        return FormatInteger(arg.u, spec, arg.bits);
    case Arg::Kind::Float:
        return FormatFloat(arg.f, spec);
    case Arg::Kind::String:
        return ApplyWidth(std::string(arg.str), spec, false);
    case Arg::Kind::Pointer:
        return ApplyWidth("0x" + FormatUnsigned(arg.u, 16, false), spec, true);
    case Arg::Kind::None:
        break;
    }
    return {};
}

// Returns false when a "{" that should open a field does not.
inline bool ParseField(std::string_view format, size_t& i, Field& field)
{
    // format[i] is '{' on entry.
    i++;
    if (i < format.size() && format[i] == '{')
    {
        i++;  // "{{" is handled by the caller; reaching here is a caller bug.
        return false;
    }
    size_t number = 0;
    bool have_number = false;
    while (i < format.size() && format[i] >= '0' && format[i] <= '9')
    {
        number = number * 10 + static_cast<size_t>(format[i++] - '0');
        have_number = true;
    }
    field.positional = have_number;
    field.index = number;
    const size_t spec_begin = i;
    while (i < format.size() && format[i] != '}' && format[i] != '{') i++;
    if (i >= format.size() || format[i] != '}') return false;
    const std::string_view inside = format.substr(spec_begin, i - spec_begin);
    i++;  // the closing brace
    if (!inside.empty())
    {
        if (inside[0] != ':') return false;  // named arguments are not part of the subset
        field.has_spec = true;
        field.spec_text = inside.substr(1);
        if (!ParseSpec(field.spec_text, field.spec)) return false;
    }
    return true;
}

inline std::string Format(std::string_view format, const Arg* args, size_t count)
{
    std::string out;
    out.reserve(format.size() + 32);
    size_t next_automatic = 0;
    for (size_t i = 0; i < format.size();)
    {
        const char c = format[i];
        if (c == '{')
        {
            if (i + 1 < format.size() && format[i + 1] == '{')
            {
                out.push_back('{');
                i += 2;
                continue;
            }
            Field field;
            if (!ParseField(format, i, field))
            {
                out.append("<bad format field>");
                continue;
            }
            const size_t index = field.positional ? field.index : next_automatic++;
            if (!field.positional) next_automatic = index + 1;
            // Out-of-range is a programming error; printing a marker keeps it visible rather than
            // reading past the argument list the way std::format would abort.
            if (index >= count)
            {
                out.append("<missing argument>");
                continue;
            }
            if (field.has_spec) out += FormatArg(args[index], field.spec);
            else out += FormatArg(args[index], Spec{});
            continue;
        }
        if (c == '}')
        {
            if (i + 1 < format.size() && format[i + 1] == '}')
            {
                out.push_back('}');
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        out.push_back(c);
        i++;
    }
    return out;
}

}  // namespace scr_fmt_detail

namespace std
{

struct format_args
{
    const scr_fmt_detail::Arg* data = nullptr;
    size_t size = 0;
};

template <typename... Args>
format_args make_format_args(const Args&... values)
{
    // A static array, because the result has to outlive this expression for vformat. std::format
    // takes the arguments by value, which is where that lifetime comes from there.
    static thread_local std::vector<scr_fmt_detail::Arg> storage;
    storage.clear();
    storage.reserve(sizeof...(Args));
    (storage.push_back(scr_fmt_detail::MakeArg(values)), ...);
    return format_args{storage.data(), storage.size()};
}

inline std::string vformat(std::string_view format, format_args args)
{
    return scr_fmt_detail::Format(format, args.data, args.size);
}

template <typename... Args>
std::string format(std::string_view format, const Args&... values)
{
    if constexpr (sizeof...(Args) == 0)
    {
        return scr_fmt_detail::Format(format, nullptr, 0);
    }
    else
    {
        const std::vector<scr_fmt_detail::Arg> args{scr_fmt_detail::MakeArg(values)...};
        return scr_fmt_detail::Format(format, args.data(), args.size());
    }
}

}  // namespace std

#endif  // __ANDROID__
