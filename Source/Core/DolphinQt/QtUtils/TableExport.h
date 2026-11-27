// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>

#include <QList>
#include <QString>
#include <QStringList>

class QAbstractItemView;
class QMenu;

// Getting the contents of the devkit's tool tables (memory map, allocations, profiler results,
// ...) out as spreadsheets: copy as CSV, save as CSV or open in the Data Tables panel.
namespace TableExport
{
// The view's visible columns and rows as text, with the column headers as the first row. Tree
// rows are flattened in display order (collapsed children included), indented by depth.
QList<QStringList> ToRows(const QAbstractItemView* view);
// A name for the table, taken from the panel and tab it's shown in ("Memory Map", "Xash3D Memory
// - Pools", ...).
QString TableName(const QAbstractItemView* view);

void CopyAsCsv(const QAbstractItemView* view);
void SaveAsCsv(QAbstractItemView* view);

// Adds "Copy Table as CSV", "Export Table to CSV..." and "Open in Data Tables" to |menu|.
void AddActions(QMenu* menu, QAbstractItemView* view);
// Gives a view that has no context menu of its own a menu with just those actions.
void Install(QAbstractItemView* view);

// Set by the main window: opens rows (header first) as a new, unsaved data table.
using OpenHandler = std::function<void(const QString& name, const QList<QStringList>& rows)>;
void SetOpenHandler(OpenHandler handler);
}  // namespace TableExport
