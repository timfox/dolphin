// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Game data tables: CSV/TSV spreadsheets that live in a homebrew project (item stats, enemy
// spawns, level lists, ...). The devkit's Data Tables panel edits them; this module parses and
// writes the files, evaluates spreadsheet formulas and bakes a table into a C header the game
// compiles in.
//
// The first line of a file is the header row: one name per column. A name may end in ":type" to
// pick the C type the column bakes to (s8, s16, s32, u8, u16, u32, hex, f32, f64, bool, str);
// otherwise the type is inferred from the column's values.
//
// A cell whose text starts with '=' is a formula: numbers (decimal or 0x hex), "strings", TRUE
// and FALSE, A1-style references (A1 is column A of the first data row, below the header), ranges
// (A1:B4), [column name] for that column in the formula's own row, the operators + - * / % ^ &
// = <> < > <= >= and functions such as SUM, AVERAGE, MIN, MAX, COUNT, IF, ROUND and HEX.
namespace GxBake::DataTable
{
using Row = std::vector<std::string>;
using Rows = std::vector<Row>;

// Parses delimited text (RFC 4180 quoting; a UTF-8 BOM, CRLF line ends and a trailing newline are
// accepted). Rows keep their own length; callers pad ragged rows if they need a rectangle.
Rows ParseDelimited(std::string_view text, char separator);
// Writes rows as delimited text, quoting only fields that need it, one '\n' per row.
std::string WriteDelimited(const Rows& rows, char separator);
// ',' for .csv, '\t' for .tsv; otherwise whichever of tab, ';' and ',' the first line uses most.
char DetectSeparator(std::string_view text, std::string_view extension);

struct Value
{
  enum class Kind
  {
    Empty,
    Number,
    Boolean,
    Text,
    Error,
  };

  Kind kind = Kind::Empty;
  double number = 0;  // Number, and Boolean as 0/1.
  std::string text;   // Text, or the error code ("#REF!", "#DIV/0!", ...).

  static Value Num(double n) { return {Kind::Number, n, {}}; }
  static Value Bool(bool b) { return {Kind::Boolean, b ? 1.0 : 0.0, {}}; }
  static Value Str(std::string s) { return {Kind::Text, 0, std::move(s)}; }
  static Value Err(std::string code) { return {Kind::Error, 0, std::move(code)}; }

  bool IsError() const { return kind == Kind::Error; }
};

// The value a cell's text stands for when it isn't a formula: empty, a number (decimal or 0x hex),
// TRUE/FALSE, or text.
Value ParseLiteral(std::string_view text);
// How a value is shown in a cell and written when a table is exported with its formulas resolved.
std::string FormatValue(const Value& value);

// Column letters for a zero-based column: 0 -> "A", 25 -> "Z", 26 -> "AA".
std::string ColumnName(size_t column);
// "B3" for column 1, data row 2 (both zero-based).
std::string CellName(size_t row, size_t column);

// Evaluates the cells of a table's data rows (everything below the header). Results are cached,
// so build one per edit and query as many cells as needed.
class Evaluator
{
public:
  // |header| names the columns (for [name] references); |data| holds the data rows.
  Evaluator(const Row& header, const Rows& data);

  Value Get(size_t row, size_t column);
  // Evaluates a formula (without its leading '=') as if it were typed into (|row|, |column|).
  Value EvaluateFormula(std::string_view formula, size_t row, size_t column);

  size_t RowCount() const { return m_data.size(); }
  size_t ColumnCount() const { return m_columns; }

private:
  friend class Parser;

  enum class State : unsigned char
  {
    NotEvaluated,
    InProgress,
    Done,
  };

  const Row& m_header;
  const Rows& m_data;
  size_t m_columns = 0;
  std::vector<Value> m_values;
  std::vector<State> m_state;
  int m_depth = 0;
};

enum class ColumnType
{
  S8,
  S16,
  S32,
  U8,
  U16,
  U32,
  Hex,  // u32 written as 0x...
  F32,
  F64,
  Bool,
  String,
};

struct Column
{
  std::string name;  // header name without the ":type" suffix
  ColumnType type = ColumnType::String;
  bool explicit_type = false;  // the header named the type
};

// Splits "damage:u8" into its name and type. Unknown suffixes are kept as part of the name.
Column ParseColumnHeader(std::string_view header);
std::string_view ColumnTypeName(ColumnType type);
// The C type a column bakes to.
std::string_view ColumnCType(ColumnType type);
// Infers each column's type from its evaluated values unless the header names one.
std::vector<Column> DescribeColumns(const Row& header, const Rows& data);

// Turns a name into a C identifier: other characters become '_', a leading digit or a C keyword
// gets an extra '_'.
std::string ToIdentifier(std::string_view name);

struct HeaderResult
{
  bool ok = false;
  std::string text;   // the header when ok
  std::string error;  // why the table can't be baked
};

// Bakes a table to a C header: a row struct, a <NAME>_COUNT constant and a static const array of
// rows named |table_name|. When the first column holds unique names, an enum of row indices is
// emitted too. |source_name| is mentioned in the "generated" comment.
HeaderResult ExportCHeader(const Row& header, const Rows& data, std::string_view table_name,
                           std::string_view source_name);
}  // namespace GxBake::DataTable
