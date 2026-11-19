// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <thread>

#include <QObject>
#include <QString>

class DevkitProject;

// The devkit toolbar's Make DOL (the project's newest ELF to a DOL) and Pack GCM (the project's
// disc folder to a GameCube disc image). Packing runs on a worker thread.
class DevkitPackager final : public QObject
{
  Q_OBJECT
public:
  explicit DevkitPackager(DevkitProject* project, QObject* parent = nullptr);
  ~DevkitPackager() override;

  bool IsPacking() const { return m_packing; }

  // Both report through OutputLine/Status. Return false when nothing was written.
  bool MakeDol();
  bool PackGcm();
  void CancelPack();

  // The ELF Make DOL converts: the run target when it is an ELF, else the newest ELF in the
  // project. Empty if there is none.
  QString FindElf() const;

signals:
  void OutputLine(const QString& line, bool is_error);
  void StatusMessage(const QString& text, bool success);
  void Progress(const QString& text);
  void PackingChanged(bool packing);

private:
  void FinishPack(bool success, const QString& output, const QString& error);

  DevkitProject* m_project;
  std::thread m_thread;
  std::atomic<bool> m_cancel = false;
  bool m_packing = false;
};
