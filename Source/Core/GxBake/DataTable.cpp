// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GxBake/DataTable.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <utility>

namespace GxBake::DataTable
{
namespace
{
constexpr int MAX_DEPTH = 512;

char ToUpper(char c)
{
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

bool IsAlpha(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool IsDigit(char c)
{
  return c >= '0' && c <= '9';
}

bool IsSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view Trim(std::string_view s)
{
  while (!s.empty() && IsSpace(s.front()))
    s.remove_prefix(1);
  while (!s.empty() && IsSpace(s.back()))
    s.remove_suffix(1);
  return s;
}

bool EqualsNoCase(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (ToUpper(a[i]) != ToUpper(b[i]))
      return false;
  }
  return true;
}

// Parses a whole string as a decimal or 0x hex number.
std::optional<double> ParseNumber(std::string_view s, bool* is_hex = nullptr)
{
  s = Trim(s);
  if (s.empty())
    return std::nullopt;

  bool negative = false;
  std::string_view digits = s;
  if (digits.front() == '-' || digits.front() == '+')
  {
    negative = digits.front() == '-';
    digits.remove_prefix(1);
  }
  if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
  {
    uint64_t value = 0;
    const auto [end, ec] = std::from_chars(digits.data() + 2, digits.data() + digits.size(), value, 16);
    if (ec != std::errc() || end != digits.data() + digits.size())
      return std::nullopt;
    if (is_hex)
      *is_hex = true;
    const double d = static_cast<double>(value);
    return negative ? -d : d;
  }

  // from_chars doesn't take a leading '+'; it also rejects "inf"/"nan" spelled oddly, which is
  // what we want for data cells.
  if (digits.empty() || !(IsDigit(digits.front()) || digits.front() == '.'))
    return std::nullopt;
  double value = 0;
  const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (ec != std::errc() || end != digits.data() + digits.size())
    return std::nullopt;
  if (is_hex)
    *is_hex = false;
  return negative ? -value : value;
}

std::string FormatNumber(double n)
{
  if (std::isnan(n))
    return "#NUM!";
  if (std::isinf(n))
    return "#NUM!";
  if (n == 0)
    return "0";
  if (std::abs(n) < 1e15 && n == std::trunc(n))
  {
    std::array<char, 32> buffer{};
    const auto [end, ec] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), static_cast<int64_t>(n));
    return std::string(buffer.data(), end);
  }
  std::array<char, 64> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), n,
                                       std::chars_format::general, 12);
  return std::string(buffer.data(), end);
}

bool IsError(const Value& v)
{
  return v.kind == Value::Kind::Error;
}

// Converts a value for arithmetic. Text that reads as a number counts; other text is #VALUE!.
std::optional<double> ToNumber(const Value& v)
{
  switch (v.kind)
  {
  case Value::Kind::Empty:
    return 0.0;
  case Value::Kind::Number:
  case Value::Kind::Boolean:
    return v.number;
  case Value::Kind::Text:
    return ParseNumber(v.text);
  case Value::Kind::Error:
    break;
  }
  return std::nullopt;
}

std::string ToText(const Value& v)
{
  switch (v.kind)
  {
  case Value::Kind::Empty:
    return {};
  case Value::Kind::Text:
    return v.text;
  default:
    return FormatValue(v);
  }
}

bool ToBool(const Value& v)
{
  if (v.kind == Value::Kind::Text)
    return EqualsNoCase(v.text, "TRUE") || (ParseNumber(v.text).value_or(0) != 0);
  return ToNumber(v).value_or(0) != 0;
}

// Parses "AB12" / "$A$1" at |s|; returns the length consumed, or 0.
size_t ParseCellRef(std::string_view s, size_t* row, size_t* column)
{
  size_t i = 0;
  if (i < s.size() && s[i] == '$')
    ++i;
  const size_t letters_start = i;
  size_t col = 0;
  while (i < s.size() && IsAlpha(s[i]))
  {
    col = col * 26 + static_cast<size_t>(ToUpper(s[i]) - 'A' + 1);
    if (col > 18278)  // past ZZZ
      return 0;
    ++i;
  }
  if (i == letters_start)
    return 0;
  if (i < s.size() && s[i] == '$')
    ++i;
  const size_t digits_start = i;
  size_t r = 0;
  while (i < s.size() && IsDigit(s[i]))
  {
    r = r * 10 + static_cast<size_t>(s[i] - '0');
    if (r > 10'000'000)
      return 0;
    ++i;
  }
  if (i == digits_start || r == 0)
    return 0;
  // "SUM(" isn't a cell reference; neither is "A1B".
  if (i < s.size() && (IsAlpha(s[i]) || s[i] == '_' || s[i] == '('))
    return 0;
  *row = r - 1;
  *column = col - 1;
  return i;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Delimited text

Rows ParseDelimited(std::string_view text, char separator)
{
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
  {
    text.remove_prefix(3);
  }

  Rows rows;
  Row row;
  std::string field;
  bool in_quotes = false;
  bool field_started = false;  // something (even "") was read for the current row
  for (size_t i = 0; i < text.size(); ++i)
  {
    const char c = text[i];
    if (in_quotes)
    {
      if (c == '"')
      {
        if (i + 1 < text.size() && text[i + 1] == '"')
        {
          field += '"';
          ++i;
        }
        else
        {
          in_quotes = false;
        }
      }
      else
      {
        field += c;
      }
      continue;
    }

    if (c == '"' && field.empty())
    {
      in_quotes = true;
      field_started = true;
    }
    else if (c == separator)
    {
      row.push_back(std::move(field));
      field.clear();
      field_started = true;
    }
    else if (c == '\n' || c == '\r')
    {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
        ++i;
      row.push_back(std::move(field));
      field.clear();
      rows.push_back(std::move(row));
      row.clear();
      field_started = false;
    }
    else
    {
      field += c;
      field_started = true;
    }
  }
  if (field_started || !field.empty() || !row.empty())
  {
    row.push_back(std::move(field));
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string WriteDelimited(const Rows& rows, char separator)
{
  std::string out;
  for (const Row& row : rows)
  {
    for (size_t i = 0; i < row.size(); ++i)
    {
      if (i != 0)
        out += separator;
      const std::string& field = row[i];
      const bool quote = field.find_first_of(std::string{separator, '"', '\n', '\r'}) !=
                             std::string::npos ||
                         (!field.empty() && (IsSpace(field.front()) || IsSpace(field.back())));
      if (!quote)
      {
        out += field;
        continue;
      }
      out += '"';
      for (const char c : field)
      {
        if (c == '"')
          out += '"';
        out += c;
      }
      out += '"';
    }
    out += '\n';
  }
  return out;
}

char DetectSeparator(std::string_view text, std::string_view extension)
{
  if (EqualsNoCase(extension, "tsv") || EqualsNoCase(extension, "tab"))
    return '\t';
  if (EqualsNoCase(extension, "csv"))
    return ',';

  const std::string_view first_line = text.substr(0, text.find('\n'));
  const auto tabs = std::count(first_line.begin(), first_line.end(), '\t');
  const auto semicolons = std::count(first_line.begin(), first_line.end(), ';');
  const auto commas = std::count(first_line.begin(), first_line.end(), ',');
  if (tabs > 0 && tabs >= semicolons && tabs >= commas)
    return '\t';
  if (semicolons > commas)
    return ';';
  return ',';
}

// ---------------------------------------------------------------------------------------------
// Values

Value ParseLiteral(std::string_view text)
{
  const std::string_view trimmed = Trim(text);
  if (trimmed.empty())
    return {};
  if (const auto number = ParseNumber(trimmed))
    return Value::Num(*number);
  if (EqualsNoCase(trimmed, "TRUE"))
    return Value::Bool(true);
  if (EqualsNoCase(trimmed, "FALSE"))
    return Value::Bool(false);
  return Value::Str(std::string(text));
}

std::string FormatValue(const Value& value)
{
  switch (value.kind)
  {
  case Value::Kind::Empty:
    return {};
  case Value::Kind::Number:
    return FormatNumber(value.number);
  case Value::Kind::Boolean:
    return value.number != 0 ? "TRUE" : "FALSE";
  case Value::Kind::Text:
  case Value::Kind::Error:
    return value.text;
  }
  return {};
}

std::string ColumnName(size_t column)
{
  std::string name;
  ++column;
  while (column > 0)
  {
    const size_t rem = (column - 1) % 26;
    name.insert(name.begin(), static_cast<char>('A' + rem));
    column = (column - 1) / 26;
  }
  return name;
}

std::string CellName(size_t row, size_t column)
{
  return ColumnName(column) + std::to_string(row + 1);
}

// ---------------------------------------------------------------------------------------------
// Formulas

namespace
{
// A function argument: one value, or a rectangular range of cells.
struct Arg
{
  bool is_range = false;
  Value value;
  size_t row0 = 0, col0 = 0, row1 = 0, col1 = 0;
};
}  // namespace

class Parser
{
public:
  Parser(Evaluator& evaluator, std::string_view text, size_t row, size_t column)
      : m_eval(evaluator), m_text(text), m_row(row), m_column(column)
  {
  }

  Value Run()
  {
    Arg result = ParseExpression();
    SkipSpace();
    if (m_error.empty() && m_pos != m_text.size())
      Fail("#PARSE!");
    if (!m_error.empty())
      return Value::Err(m_error);
    return Collapse(result);
  }

private:
  void Fail(const char* code)
  {
    if (m_error.empty())
      m_error = code;
  }

  void SkipSpace()
  {
    while (m_pos < m_text.size() && IsSpace(m_text[m_pos]))
      ++m_pos;
  }

  bool Accept(std::string_view token)
  {
    SkipSpace();
    if (m_text.substr(m_pos, token.size()) == token)
    {
      m_pos += token.size();
      return true;
    }
    return false;
  }

  char Peek()
  {
    SkipSpace();
    return m_pos < m_text.size() ? m_text[m_pos] : '\0';
  }

  Value CellValue(size_t row, size_t column)
  {
    if (row >= m_eval.RowCount() || column >= m_eval.ColumnCount())
      return Value::Err("#REF!");
    return m_eval.Get(row, column);
  }

  // A range used where one value is expected: a single cell works, anything bigger is #VALUE!.
  Value Collapse(const Arg& arg)
  {
    if (!arg.is_range)
      return arg.value;
    if (arg.row0 == arg.row1 && arg.col0 == arg.col1)
      return CellValue(arg.row0, arg.col0);
    return Value::Err("#VALUE!");
  }

  template <typename F>
  void ForEachInRange(const Arg& arg, F&& f)
  {
    if (!arg.is_range)
    {
      f(arg.value, false);
      return;
    }
    for (size_t r = arg.row0; r <= arg.row1; ++r)
    {
      for (size_t c = arg.col0; c <= arg.col1; ++c)
        f(CellValue(r, c), true);
    }
  }

  Arg Single(Value v)
  {
    Arg arg;
    arg.value = std::move(v);
    return arg;
  }

  Arg ParseExpression() { return ParseComparison(); }

  Arg ParseComparison()
  {
    Arg left = ParseConcat();
    while (m_error.empty())
    {
      int op;
      if (Accept("<="))
        op = 0;
      else if (Accept(">="))
        op = 1;
      else if (Accept("<>"))
        op = 2;
      else if (Accept("<"))
        op = 3;
      else if (Accept(">"))
        op = 4;
      else if (Accept("="))
        op = 5;
      else
        break;
      const Value a = Collapse(left);
      const Value b = Collapse(ParseConcat());
      if (IsError(a))
        continue;
      if (IsError(b))
      {
        left = Single(b);
        continue;
      }
      int cmp;
      const auto na = (a.kind == Value::Kind::Text) ? std::nullopt : ToNumber(a);
      const auto nb = (b.kind == Value::Kind::Text) ? std::nullopt : ToNumber(b);
      if (na && nb)
      {
        cmp = (*na < *nb) ? -1 : (*na > *nb ? 1 : 0);
      }
      else
      {
        std::string ta = ToText(a), tb = ToText(b);
        std::transform(ta.begin(), ta.end(), ta.begin(), ToUpper);
        std::transform(tb.begin(), tb.end(), tb.begin(), ToUpper);
        cmp = ta.compare(tb);
        cmp = (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
      }
      bool result = false;
      switch (op)
      {
      case 0:
        result = cmp <= 0;
        break;
      case 1:
        result = cmp >= 0;
        break;
      case 2:
        result = cmp != 0;
        break;
      case 3:
        result = cmp < 0;
        break;
      case 4:
        result = cmp > 0;
        break;
      default:
        result = cmp == 0;
        break;
      }
      left = Single(Value::Bool(result));
    }
    return left;
  }

  Arg ParseConcat()
  {
    Arg left = ParseAdditive();
    while (m_error.empty() && Accept("&"))
    {
      const Value a = Collapse(left);
      const Value b = Collapse(ParseAdditive());
      if (IsError(a))
        continue;
      left = Single(IsError(b) ? b : Value::Str(ToText(a) + ToText(b)));
    }
    return left;
  }

  Value Arith(const Value& a, const Value& b, char op)
  {
    if (IsError(a))
      return a;
    if (IsError(b))
      return b;
    const auto na = ToNumber(a);
    const auto nb = ToNumber(b);
    if (!na || !nb)
      return Value::Err("#VALUE!");
    switch (op)
    {
    case '+':
      return Value::Num(*na + *nb);
    case '-':
      return Value::Num(*na - *nb);
    case '*':
      return Value::Num(*na * *nb);
    case '/':
      if (*nb == 0)
        return Value::Err("#DIV/0!");
      return Value::Num(*na / *nb);
    case '%':
      if (*nb == 0)
        return Value::Err("#DIV/0!");
      return Value::Num(*na - *nb * std::floor(*na / *nb));
    case '^':
    {
      const double r = std::pow(*na, *nb);
      if (!std::isfinite(r))
        return Value::Err("#NUM!");
      return Value::Num(r);
    }
    }
    return Value::Err("#VALUE!");
  }

  Arg ParseAdditive()
  {
    Arg left = ParseTerm();
    while (m_error.empty())
    {
      const char c = Peek();
      if (c != '+' && c != '-')
        break;
      ++m_pos;
      const Value b = Collapse(ParseTerm());
      left = Single(Arith(Collapse(left), b, c));
    }
    return left;
  }

  Arg ParseTerm()
  {
    Arg left = ParsePower();
    while (m_error.empty())
    {
      const char c = Peek();
      if (c != '*' && c != '/' && c != '%')
        break;
      ++m_pos;
      const Value b = Collapse(ParsePower());
      left = Single(Arith(Collapse(left), b, c));
    }
    return left;
  }

  Arg ParsePower()
  {
    Arg left = ParseUnary();
    while (m_error.empty() && Peek() == '^')
    {
      ++m_pos;
      const Value b = Collapse(ParseUnary());
      left = Single(Arith(Collapse(left), b, '^'));
    }
    return left;
  }

  Arg ParseUnary()
  {
    const char c = Peek();
    if (c == '-' || c == '+')
    {
      ++m_pos;
      const Value v = Collapse(ParseUnary());
      if (c == '+')
        return Single(v);
      return Single(Arith(Value::Num(0), v, '-'));
    }
    return ParsePrimary();
  }

  Arg ParsePrimary()
  {
    const char c = Peek();
    if (c == '\0')
    {
      Fail("#PARSE!");
      return {};
    }

    if (c == '(')
    {
      ++m_pos;
      Arg inner = ParseExpression();
      if (!Accept(")"))
        Fail("#PARSE!");
      return inner;
    }

    if (c == '"')
    {
      ++m_pos;
      std::string s;
      while (true)
      {
        if (m_pos >= m_text.size())
        {
          Fail("#PARSE!");
          return {};
        }
        const char ch = m_text[m_pos++];
        if (ch == '"')
        {
          if (m_pos < m_text.size() && m_text[m_pos] == '"')
          {
            s += '"';
            ++m_pos;
            continue;
          }
          break;
        }
        s += ch;
      }
      return Single(Value::Str(std::move(s)));
    }

    if (c == '[')
    {
      ++m_pos;
      const size_t end = m_text.find(']', m_pos);
      if (end == std::string_view::npos)
      {
        Fail("#PARSE!");
        return {};
      }
      const std::string_view name = Trim(m_text.substr(m_pos, end - m_pos));
      m_pos = end + 1;
      for (size_t col = 0; col < m_eval.m_header.size(); ++col)
      {
        const std::string_view header = m_eval.m_header[col];
        if (EqualsNoCase(Trim(header), name) ||
            EqualsNoCase(ParseColumnHeader(header).name, name))
        {
          Arg arg;
          arg.is_range = true;
          arg.row0 = arg.row1 = m_row;
          arg.col0 = arg.col1 = col;
          return arg;
        }
      }
      return Single(Value::Err("#NAME?"));
    }

    if (IsDigit(c) || c == '.')
    {
      size_t end = m_pos;
      if (c == '0' && end + 1 < m_text.size() && (m_text[end + 1] == 'x' || m_text[end + 1] == 'X'))
      {
        end += 2;
        while (end < m_text.size() && std::isxdigit(static_cast<unsigned char>(m_text[end])))
          ++end;
      }
      else
      {
        while (end < m_text.size() && (IsDigit(m_text[end]) || m_text[end] == '.'))
          ++end;
        if (end < m_text.size() && (m_text[end] == 'e' || m_text[end] == 'E'))
        {
          size_t exp = end + 1;
          if (exp < m_text.size() && (m_text[exp] == '+' || m_text[exp] == '-'))
            ++exp;
          if (exp < m_text.size() && IsDigit(m_text[exp]))
          {
            end = exp;
            while (end < m_text.size() && IsDigit(m_text[end]))
              ++end;
          }
        }
      }
      const auto number = ParseNumber(m_text.substr(m_pos, end - m_pos));
      if (!number)
      {
        Fail("#PARSE!");
        return {};
      }
      m_pos = end;
      return Single(Value::Num(*number));
    }

    if (IsAlpha(c) || c == '$' || c == '_')
    {
      size_t row = 0, column = 0;
      if (const size_t length = ParseCellRef(m_text.substr(m_pos), &row, &column))
      {
        m_pos += length;
        Arg arg;
        arg.is_range = true;
        arg.row0 = arg.row1 = row;
        arg.col0 = arg.col1 = column;
        if (Accept(":"))
        {
          SkipSpace();
          size_t row2 = 0, column2 = 0;
          const size_t length2 = ParseCellRef(m_text.substr(m_pos), &row2, &column2);
          if (!length2)
          {
            Fail("#PARSE!");
            return {};
          }
          m_pos += length2;
          arg.row0 = std::min(row, row2);
          arg.row1 = std::max(row, row2);
          arg.col0 = std::min(column, column2);
          arg.col1 = std::max(column, column2);
        }
        return arg;
      }

      size_t end = m_pos;
      while (end < m_text.size() &&
             (IsAlpha(m_text[end]) || IsDigit(m_text[end]) || m_text[end] == '_' ||
              m_text[end] == '.'))
      {
        ++end;
      }
      std::string name(m_text.substr(m_pos, end - m_pos));
      std::transform(name.begin(), name.end(), name.begin(), ToUpper);
      m_pos = end;
      if (Accept("("))
        return Single(CallFunction(name));
      if (name == "TRUE")
        return Single(Value::Bool(true));
      if (name == "FALSE")
        return Single(Value::Bool(false));
      return Single(Value::Err("#NAME?"));
    }

    Fail("#PARSE!");
    return {};
  }

  std::vector<Arg> ParseArgs()
  {
    std::vector<Arg> args;
    if (Accept(")"))
      return args;
    while (m_error.empty())
    {
      args.push_back(ParseExpression());
      if (Accept(")"))
        break;
      if (!Accept(",") && !Accept(";"))
      {
        Fail("#PARSE!");
        break;
      }
    }
    return args;
  }

  Value CallFunction(const std::string& name)
  {
    // Cell and range arguments are only read when a function uses them, so IF doesn't evaluate
    // (or report errors from) the branch it doesn't take.
    std::vector<Arg> args = ParseArgs();
    if (!m_error.empty())
      return {};

    const auto need = [&](size_t min, size_t max) { return args.size() >= min && args.size() <= max; };
    const auto num_arg = [&](size_t i, double* out) -> std::optional<Value> {
      const Value v = Collapse(args[i]);
      if (IsError(v))
        return v;
      const auto n = ToNumber(v);
      if (!n)
        return Value::Err("#VALUE!");
      *out = *n;
      return std::nullopt;
    };

    if (name == "SUM" || name == "AVERAGE" || name == "AVG" || name == "MIN" || name == "MAX" ||
        name == "COUNT" || name == "COUNTA" || name == "PRODUCT")
    {
      double sum = 0, product = 1;
      double lo = std::numeric_limits<double>::infinity();
      double hi = -std::numeric_limits<double>::infinity();
      size_t count = 0, counta = 0;
      std::optional<Value> error;
      for (const Arg& arg : args)
      {
        ForEachInRange(arg, [&](const Value& v, bool from_range) {
          if (error)
            return;
          if (v.kind != Value::Kind::Empty)
            ++counta;
          if (IsError(v))
          {
            if (name != "COUNT" && name != "COUNTA")
              error = v;
            return;
          }
          std::optional<double> n;
          if (v.kind == Value::Kind::Number)
            n = v.number;
          else if (!from_range && v.kind != Value::Kind::Empty)
            n = ToNumber(v);  // literal arguments like SUM("3", TRUE) count
          if (!n)
            return;
          ++count;
          sum += *n;
          product *= *n;
          lo = std::min(lo, *n);
          hi = std::max(hi, *n);
        });
      }
      if (error)
        return *error;
      if (name == "SUM")
        return Value::Num(sum);
      if (name == "PRODUCT")
        return Value::Num(count ? product : 0);
      if (name == "COUNT")
        return Value::Num(static_cast<double>(count));
      if (name == "COUNTA")
        return Value::Num(static_cast<double>(counta));
      if (name == "MIN")
        return Value::Num(count ? lo : 0);
      if (name == "MAX")
        return Value::Num(count ? hi : 0);
      if (count == 0)
        return Value::Err("#DIV/0!");
      return Value::Num(sum / static_cast<double>(count));
    }

    if (name == "IF")
    {
      if (!need(2, 3))
        return Value::Err("#VALUE!");
      const Value cond = Collapse(args[0]);
      if (IsError(cond))
        return cond;
      if (ToBool(cond))
        return Collapse(args[1]);
      return args.size() == 3 ? Collapse(args[2]) : Value::Bool(false);
    }

    if (name == "AND" || name == "OR")
    {
      bool result = name == "AND";
      std::optional<Value> error;
      for (const Arg& arg : args)
      {
        ForEachInRange(arg, [&](const Value& v, bool) {
          if (error || v.kind == Value::Kind::Empty)
            return;
          if (IsError(v))
          {
            error = v;
            return;
          }
          if (name == "AND")
            result = result && ToBool(v);
          else
            result = result || ToBool(v);
        });
      }
      if (error)
        return *error;
      return Value::Bool(result);
    }

    if (name == "NOT")
    {
      if (!need(1, 1))
        return Value::Err("#VALUE!");
      const Value v = Collapse(args[0]);
      return IsError(v) ? v : Value::Bool(!ToBool(v));
    }

    if (name == "ROW" || name == "COLUMN" || name == "COL")
    {
      const bool row = name == "ROW";
      if (args.empty())
        return Value::Num(static_cast<double>((row ? m_row : m_column) + 1));
      if (!args[0].is_range)
        return Value::Err("#VALUE!");
      return Value::Num(static_cast<double>((row ? args[0].row0 : args[0].col0) + 1));
    }

    if (name == "CONCAT" || name == "CONCATENATE")
    {
      std::string s;
      std::optional<Value> error;
      for (const Arg& arg : args)
      {
        ForEachInRange(arg, [&](const Value& v, bool) {
          if (error)
            return;
          if (IsError(v))
            error = v;
          else
            s += ToText(v);
        });
      }
      return error ? *error : Value::Str(std::move(s));
    }

    if (name == "LEN" || name == "UPPER" || name == "LOWER" || name == "TRIM")
    {
      if (!need(1, 1))
        return Value::Err("#VALUE!");
      const Value v = Collapse(args[0]);
      if (IsError(v))
        return v;
      std::string s = ToText(v);
      if (name == "LEN")
        return Value::Num(static_cast<double>(s.size()));
      if (name == "TRIM")
        return Value::Str(std::string(Trim(s)));
      std::transform(s.begin(), s.end(), s.begin(), [&](char ch) {
        if (name == "UPPER")
          return ToUpper(ch);
        return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
      });
      return Value::Str(std::move(s));
    }

    if (name == "HEX")
    {
      if (!need(1, 2))
        return Value::Err("#VALUE!");
      double n = 0, digits = 0;
      if (auto e = num_arg(0, &n))
        return *e;
      if (args.size() == 2)
      {
        if (auto e = num_arg(1, &digits))
          return *e;
      }
      if (n < 0 || n > 18446744073709551615.0 || digits < 0 || digits > 16)
        return Value::Err("#NUM!");
      std::array<char, 32> buffer{};
      const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                           static_cast<uint64_t>(n), 16);
      std::string hex(buffer.data(), end);
      std::transform(hex.begin(), hex.end(), hex.begin(), ToUpper);
      if (hex.size() < static_cast<size_t>(digits))
        hex.insert(0, static_cast<size_t>(digits) - hex.size(), '0');
      return Value::Str("0x" + hex);
    }

    // Single-number functions.
    using Fn1 = double (*)(double);
    static const std::pair<const char*, Fn1> unary[] = {
        {"ABS", [](double x) { return std::abs(x); }},
        {"INT", [](double x) { return std::floor(x); }},
        {"FLOOR", [](double x) { return std::floor(x); }},
        {"CEIL", [](double x) { return std::ceil(x); }},
        {"CEILING", [](double x) { return std::ceil(x); }},
        {"SQRT", [](double x) { return std::sqrt(x); }},
        {"SIN", [](double x) { return std::sin(x); }},
        {"COS", [](double x) { return std::cos(x); }},
        {"TAN", [](double x) { return std::tan(x); }},
        {"RADIANS", [](double x) { return x * 3.14159265358979323846 / 180.0; }},
        {"DEGREES", [](double x) { return x * 180.0 / 3.14159265358979323846; }},
    };
    for (const auto& [fn_name, fn] : unary)
    {
      if (name != fn_name)
        continue;
      if (!need(1, 1))
        return Value::Err("#VALUE!");
      double x = 0;
      if (auto e = num_arg(0, &x))
        return *e;
      const double r = fn(x);
      if (!std::isfinite(r))
        return Value::Err("#NUM!");
      return Value::Num(r);
    }

    if (name == "ROUND")
    {
      if (!need(1, 2))
        return Value::Err("#VALUE!");
      double x = 0, digits = 0;
      if (auto e = num_arg(0, &x))
        return *e;
      if (args.size() == 2)
      {
        if (auto e = num_arg(1, &digits))
          return *e;
      }
      const double scale = std::pow(10.0, std::trunc(digits));
      return Value::Num(std::round(x * scale) / scale);
    }

    if (name == "POW" || name == "POWER" || name == "MOD")
    {
      if (!need(2, 2))
        return Value::Err("#VALUE!");
      return Arith(Collapse(args[0]), Collapse(args[1]), name == "MOD" ? '%' : '^');
    }

    if (name == "CLAMP")
    {
      if (!need(3, 3))
        return Value::Err("#VALUE!");
      double x = 0, lo = 0, hi = 0;
      if (auto e = num_arg(0, &x))
        return *e;
      if (auto e = num_arg(1, &lo))
        return *e;
      if (auto e = num_arg(2, &hi))
        return *e;
      return Value::Num(std::min(std::max(x, lo), hi));
    }

    return Value::Err("#NAME?");
  }

  Evaluator& m_eval;
  std::string_view m_text;
  size_t m_pos = 0;
  size_t m_row;
  size_t m_column;
  std::string m_error;
};

Evaluator::Evaluator(const Row& header, const Rows& data) : m_header(header), m_data(data)
{
  m_columns = header.size();
  for (const Row& row : data)
    m_columns = std::max(m_columns, row.size());
  m_values.resize(m_data.size() * m_columns);
  m_state.resize(m_data.size() * m_columns, State::NotEvaluated);
}

Value Evaluator::Get(size_t row, size_t column)
{
  if (row >= m_data.size() || column >= m_columns)
    return Value::Err("#REF!");

  const size_t index = row * m_columns + column;
  switch (m_state[index])
  {
  case State::Done:
    return m_values[index];
  case State::InProgress:
    return Value::Err("#CYCLE!");
  case State::NotEvaluated:
    break;
  }

  const Row& cells = m_data[row];
  const std::string_view text = column < cells.size() ? std::string_view(cells[column]) : "";
  if (text.empty() || text.front() != '=')
  {
    m_values[index] = ParseLiteral(text);
    m_state[index] = State::Done;
    return m_values[index];
  }

  if (m_depth >= MAX_DEPTH)
    return Value::Err("#DEPTH!");  // not cached: a shallower evaluation may succeed

  m_state[index] = State::InProgress;
  ++m_depth;
  Value value = EvaluateFormula(text.substr(1), row, column);
  --m_depth;
  if (value.kind == Value::Kind::Error && value.text == "#DEPTH!")
  {
    m_state[index] = State::NotEvaluated;
    return value;
  }
  m_values[index] = std::move(value);
  m_state[index] = State::Done;
  return m_values[index];
}

Value Evaluator::EvaluateFormula(std::string_view formula, size_t row, size_t column)
{
  Parser parser(*this, formula, row, column);
  Value value = parser.Run();
  // A formula whose result is a reference to an empty cell shows 0, like other spreadsheets.
  if (value.kind == Value::Kind::Empty)
    return Value::Num(0);
  return value;
}

// ---------------------------------------------------------------------------------------------
// Columns and C export

namespace
{
struct TypeName
{
  const char* name;
  ColumnType type;
};

constexpr TypeName TYPE_NAMES[] = {
    {"s8", ColumnType::S8},       {"s16", ColumnType::S16},    {"s32", ColumnType::S32},
    {"int", ColumnType::S32},     {"u8", ColumnType::U8},      {"u16", ColumnType::U16},
    {"u32", ColumnType::U32},     {"hex", ColumnType::Hex},    {"f32", ColumnType::F32},
    {"float", ColumnType::F32},   {"f64", ColumnType::F64},    {"double", ColumnType::F64},
    {"bool", ColumnType::Bool},   {"str", ColumnType::String}, {"string", ColumnType::String},
};

bool IntegerFits(double v, double lo, double hi)
{
  return v == std::trunc(v) && v >= lo && v <= hi;
}

bool CheckRange(ColumnType type, double v)
{
  switch (type)
  {
  case ColumnType::S8:
    return IntegerFits(v, -128, 127);
  case ColumnType::S16:
    return IntegerFits(v, -32768, 32767);
  case ColumnType::S32:
    return IntegerFits(v, -2147483648.0, 2147483647.0);
  case ColumnType::U8:
    return IntegerFits(v, 0, 255);
  case ColumnType::U16:
    return IntegerFits(v, 0, 65535);
  case ColumnType::U32:
  case ColumnType::Hex:
    return IntegerFits(v, 0, 4294967295.0);
  default:
    return true;
  }
}

const std::set<std::string_view>& CKeywords()
{
  static const std::set<std::string_view> keywords = {
      "auto",     "bool",   "break",    "case",     "char",   "const",   "continue", "default",
      "do",       "double", "else",     "enum",     "extern", "float",   "for",      "goto",
      "if",       "inline", "int",      "long",     "register", "restrict", "return", "short",
      "signed",   "sizeof", "static",   "struct",   "switch", "typedef", "union",    "unsigned",
      "void",     "volatile", "while",  "true",     "false",  "NULL",    "u8",       "u16",
      "u32",      "s8",     "s16",      "s32",      "f32",    "f64"};
  return keywords;
}

void AppendCString(std::string* out, std::string_view s)
{
  *out += '"';
  for (const char ch : s)
  {
    const auto c = static_cast<unsigned char>(ch);
    switch (c)
    {
    case '"':
      *out += "\\\"";
      break;
    case '\\':
      *out += "\\\\";
      break;
    case '\n':
      *out += "\\n";
      break;
    case '\r':
      *out += "\\r";
      break;
    case '\t':
      *out += "\\t";
      break;
    default:
      if (c < 0x20 || c == 0x7f)
      {
        // Octal escapes stop after three digits, unlike \x which would swallow following hex.
        *out += '\\';
        *out += static_cast<char>('0' + ((c >> 6) & 7));
        *out += static_cast<char>('0' + ((c >> 3) & 7));
        *out += static_cast<char>('0' + (c & 7));
      }
      else
      {
        *out += ch;  // UTF-8 passes through
      }
      break;
    }
  }
  *out += '"';
}

std::string CFloat(double v, bool single)
{
  std::array<char, 64> buffer{};
  const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), v,
                                       std::chars_format::general, single ? 9 : 17);
  std::string s(buffer.data(), end);
  if (s.find_first_of(".eE") == std::string::npos)
    s += ".0";
  if (single)
    s += 'f';
  return s;
}

std::string ToUpperIdentifier(std::string_view s)
{
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), ToUpper);
  return out;
}
}  // namespace

Column ParseColumnHeader(std::string_view header)
{
  Column column;
  const std::string_view trimmed = Trim(header);
  const size_t colon = trimmed.rfind(':');
  if (colon != std::string_view::npos)
  {
    const std::string_view suffix = Trim(trimmed.substr(colon + 1));
    for (const TypeName& t : TYPE_NAMES)
    {
      if (EqualsNoCase(suffix, t.name))
      {
        column.name = std::string(Trim(trimmed.substr(0, colon)));
        column.type = t.type;
        column.explicit_type = true;
        return column;
      }
    }
  }
  column.name = std::string(trimmed);
  return column;
}

std::string_view ColumnTypeName(ColumnType type)
{
  switch (type)
  {
  case ColumnType::S8:
    return "s8";
  case ColumnType::S16:
    return "s16";
  case ColumnType::S32:
    return "s32";
  case ColumnType::U8:
    return "u8";
  case ColumnType::U16:
    return "u16";
  case ColumnType::U32:
    return "u32";
  case ColumnType::Hex:
    return "hex";
  case ColumnType::F32:
    return "f32";
  case ColumnType::F64:
    return "f64";
  case ColumnType::Bool:
    return "bool";
  case ColumnType::String:
    return "str";
  }
  return "str";
}

std::string_view ColumnCType(ColumnType type)
{
  switch (type)
  {
  case ColumnType::S8:
    return "int8_t";
  case ColumnType::S16:
    return "int16_t";
  case ColumnType::S32:
    return "int32_t";
  case ColumnType::U8:
    return "uint8_t";
  case ColumnType::U16:
    return "uint16_t";
  case ColumnType::U32:
  case ColumnType::Hex:
    return "uint32_t";
  case ColumnType::F32:
    return "float";
  case ColumnType::F64:
    return "double";
  case ColumnType::Bool:
    return "bool";
  case ColumnType::String:
    return "const char*";
  }
  return "const char*";
}

std::vector<Column> DescribeColumns(const Row& header, const Rows& data)
{
  Evaluator eval(header, data);
  std::vector<Column> columns;
  columns.reserve(eval.ColumnCount());
  for (size_t col = 0; col < eval.ColumnCount(); ++col)
  {
    Column column = ParseColumnHeader(col < header.size() ? std::string_view(header[col]) : "");
    if (column.explicit_type)
    {
      columns.push_back(std::move(column));
      continue;
    }

    bool any = false, all_bool = true, all_number = true, all_integer = true, any_hex = false;
    bool fits_s32 = true;
    for (size_t row = 0; row < eval.RowCount(); ++row)
    {
      const Value v = eval.Get(row, col);
      if (v.kind == Value::Kind::Empty)
        continue;
      any = true;
      if (v.kind != Value::Kind::Boolean)
        all_bool = false;
      // Hex literals ("0x80"), and formulas that produce them with HEX(), make a hex column.
      bool is_hex = false;
      std::optional<double> n;
      if (v.kind == Value::Kind::Number)
        n = v.number;
      else if (v.kind == Value::Kind::Text)
        n = ParseNumber(v.text, &is_hex);
      if (!n)
      {
        all_number = false;
        continue;
      }
      if (v.kind == Value::Kind::Number && col < data[row].size())
        ParseNumber(data[row][col], &is_hex);
      any_hex = any_hex || is_hex;
      if (*n != std::trunc(*n))
        all_integer = false;
      if (!CheckRange(ColumnType::S32, *n))
        fits_s32 = false;
    }

    if (!any)
      column.type = ColumnType::String;
    else if (all_bool)
      column.type = ColumnType::Bool;
    else if (all_number && all_integer && any_hex)
      column.type = ColumnType::Hex;
    else if (all_number && all_integer && fits_s32)
      column.type = ColumnType::S32;
    else if (all_number)
      column.type = ColumnType::F32;
    else
      column.type = ColumnType::String;
    columns.push_back(std::move(column));
  }
  return columns;
}

std::string ToIdentifier(std::string_view name)
{
  std::string id;
  for (const char c : Trim(name))
    id += (IsAlpha(c) || IsDigit(c) || c == '_') ? c : '_';
  if (id.empty())
    id = "_";
  if (IsDigit(id.front()))
    id.insert(id.begin(), '_');
  if (CKeywords().count(id))
    id += '_';
  return id;
}

HeaderResult ExportCHeader(const Row& header, const Rows& data, std::string_view table_name,
                           std::string_view source_name)
{
  HeaderResult result;
  const std::vector<Column> columns = DescribeColumns(header, data);
  if (columns.empty())
  {
    result.error = "The table has no columns.";
    return result;
  }

  // Field names: unique C identifiers.
  std::vector<std::string> fields;
  std::set<std::string> used;
  for (size_t i = 0; i < columns.size(); ++i)
  {
    std::string base = ToIdentifier(columns[i].name.empty() ? ColumnName(i) : columns[i].name);
    std::string field = base;
    for (int n = 2; used.count(field); ++n)
      field = base + "_" + std::to_string(n);
    used.insert(field);
    fields.push_back(std::move(field));
  }

  // Leave out rows that are completely empty (trailing blank lines in the file, spacer rows).
  std::vector<size_t> rows;
  for (size_t r = 0; r < data.size(); ++r)
  {
    if (std::any_of(data[r].begin(), data[r].end(),
                    [](const std::string& s) { return !Trim(s).empty(); }))
    {
      rows.push_back(r);
    }
  }

  Evaluator eval(header, data);
  const std::string table = ToIdentifier(table_name);
  const std::string upper = ToUpperIdentifier(table);
  const std::string row_type = table + "_row";

  // Every value, checked before anything is written so errors name the cell.
  std::vector<std::vector<Value>> values(rows.size());
  for (size_t i = 0; i < rows.size(); ++i)
  {
    for (size_t col = 0; col < columns.size(); ++col)
    {
      Value v = eval.Get(rows[i], col);
      const std::string cell = CellName(rows[i], col);
      if (v.IsError())
      {
        result.error = cell + " is " + v.text;
        return result;
      }
      const ColumnType type = columns[col].type;
      if (type != ColumnType::String && v.kind != Value::Kind::Empty)
      {
        const auto n = ToNumber(v);
        if (!n && !(type == ColumnType::Bool && v.kind == Value::Kind::Text &&
                    (EqualsNoCase(v.text, "TRUE") || EqualsNoCase(v.text, "FALSE"))))
        {
          result.error = cell + " (\"" + ToText(v) + "\") isn't a number, but column " +
                         ColumnName(col) + " (" + columns[col].name + ") is " +
                         std::string(ColumnTypeName(type)) + ".";
          return result;
        }
        if (n && !CheckRange(type, *n))
        {
          result.error = cell + " (" + FormatValue(v) + ") doesn't fit column " +
                         ColumnName(col) + " (" + columns[col].name + "), which is " +
                         std::string(ColumnTypeName(type)) + ".";
          return result;
        }
      }
      values[i].push_back(std::move(v));
    }
  }

  std::string& out = result.text;
  out += "// Generated by the Dolphin devkit from ";
  out += source_name;
  out += ".\n// Edit the data table instead: this file is rewritten whenever the table is saved.\n\n";
  out += "#pragma once\n\n#include <stdbool.h>\n#include <stdint.h>\n\n";

  // Row index enum from a first column of unique names.
  if (columns[0].type == ColumnType::String && !rows.empty())
  {
    std::vector<std::string> names;
    std::set<std::string> seen;
    bool unique = true;
    for (size_t i = 0; i < rows.size() && unique; ++i)
    {
      const std::string text = ToText(values[i][0]);
      if (Trim(text).empty())
      {
        unique = false;
        break;
      }
      const std::string id = upper + "_" + ToUpperIdentifier(ToIdentifier(text));
      unique = seen.insert(id).second;
      names.push_back(id);
    }
    if (unique)
    {
      out += "enum " + table + "_id\n{\n";
      for (size_t i = 0; i < names.size(); ++i)
        out += "  " + names[i] + " = " + std::to_string(i) + ",\n";
      out += "};\n\n";
    }
  }

  out += "typedef struct " + row_type + "\n{\n";
  for (size_t col = 0; col < columns.size(); ++col)
  {
    out += "  ";
    out += ColumnCType(columns[col].type);
    out += ' ';
    out += fields[col];
    out += ";\n";
  }
  out += "} " + row_type + ";\n\n";
  out += "#define " + upper + "_COUNT " + std::to_string(rows.size()) + "\n\n";

  if (rows.empty())
  {
    // A zero-length array isn't valid C; keep the declaration so code still compiles.
    out += "static const " + row_type + "* const " + table + " = 0;\n";
    result.ok = true;
    return result;
  }

  out += "static const " + row_type + " " + table + "[" + upper + "_COUNT] = {\n";
  for (size_t i = 0; i < rows.size(); ++i)
  {
    out += "    {";
    for (size_t col = 0; col < columns.size(); ++col)
    {
      if (col != 0)
        out += ", ";
      const Value& v = values[i][col];
      const ColumnType type = columns[col].type;
      if (type == ColumnType::String)
      {
        AppendCString(&out, ToText(v));
        continue;
      }
      double n = 0;
      if (type == ColumnType::Bool && v.kind == Value::Kind::Text)
        n = EqualsNoCase(v.text, "TRUE") ? 1 : 0;
      else
        n = ToNumber(v).value_or(0);
      switch (type)
      {
      case ColumnType::Bool:
        out += n != 0 ? "true" : "false";
        break;
      case ColumnType::F32:
        out += CFloat(n, true);
        break;
      case ColumnType::F64:
        out += CFloat(n, false);
        break;
      case ColumnType::Hex:
      {
        std::array<char, 16> buffer{};
        const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                             static_cast<uint32_t>(n), 16);
        std::string hex(buffer.data(), end);
        std::transform(hex.begin(), hex.end(), hex.begin(), ToUpper);
        hex.insert(0, 8 - std::min<size_t>(8, hex.size()), '0');
        out += "0x" + hex;
        break;
      }
      case ColumnType::U32:
        out += std::to_string(static_cast<uint32_t>(n)) + "u";
        break;
      case ColumnType::S32:
        // -2147483648 isn't a valid int literal on its own.
        if (n == -2147483648.0)
          out += "(-2147483647 - 1)";
        else
          out += std::to_string(static_cast<int64_t>(n));
        break;
      default:
        out += std::to_string(static_cast<int64_t>(n));
        break;
      }
    }
    out += "}, // " + std::to_string(rows[i] + 1) + "\n";
  }
  out += "};\n";
  result.ok = true;
  return result;
}
}  // namespace GxBake::DataTable
