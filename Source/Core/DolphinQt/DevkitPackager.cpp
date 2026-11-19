// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitPackager.h"

#include <memory>
#include <string>
#include <vector>

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>

#include "Core/Boot/ElfToDol.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DirectoryBlob.h"
#include "DolphinQt/DevkitProject.h"

namespace
{
QString Bytes(qint64 size)
{
  const QLocale locale;
  if (size >= 1024 * 1024 * 1024)
    return QStringLiteral("%1 GB").arg(size / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
  return QStringLiteral("%1 bytes").arg(locale.toString(size));
}
}  // namespace

DevkitPackager::DevkitPackager(DevkitProject* project, QObject* parent)
    : QObject(parent), m_project(project)
{
}

DevkitPackager::~DevkitPackager()
{
  m_cancel = true;
  if (m_thread.joinable())
    m_thread.join();
}

QString DevkitPackager::FindElf() const
{
  if (!m_project->IsOpen())
    return {};
  const QString target = m_project->ResolveRunTarget();
  if (target.endsWith(QStringLiteral(".elf"), Qt::CaseInsensitive))
    return target;

  const QDir root(m_project->GetPath());
  QFileInfo best;
  QDirIterator it(m_project->GetPath(), {QStringLiteral("*.elf")}, QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext())
  {
    const QFileInfo candidate(it.next());
    if (root.relativeFilePath(candidate.absoluteFilePath()).count(QLatin1Char('/')) > 3)
      continue;
    if (!best.exists() || candidate.lastModified() > best.lastModified())
      best = candidate;
  }
  return best.exists() ? best.absoluteFilePath() : QString();
}

bool DevkitPackager::MakeDol()
{
  if (!m_project->IsOpen())
    return false;

  const QString elf_path = FindElf();
  if (elf_path.isEmpty())
  {
    emit OutputLine(tr("[LINK] No ELF in the project. Build it first, or set an ELF as the run "
                       "target."),
                    true);
    emit StatusMessage(tr("Make DOL failed: no ELF to convert"), false);
    return false;
  }

  QFile elf_file(elf_path);
  if (!elf_file.open(QFile::ReadOnly))
  {
    emit OutputLine(tr("[LINK] Couldn't read %1").arg(QDir::toNativeSeparators(elf_path)), true);
    emit StatusMessage(tr("Make DOL failed"), false);
    return false;
  }
  const QByteArray bytes = elf_file.readAll();
  const std::vector<u8> elf(bytes.begin(), bytes.end());

  std::string error;
  Boot::DolInfo info;
  const auto dol = Boot::ElfToDol(elf, &error, &info);
  const QString elf_name = QFileInfo(elf_path).fileName();
  if (!dol)
  {
    emit OutputLine(tr("[LINK] %1: %2").arg(elf_name, QString::fromStdString(error)), true);
    emit StatusMessage(tr("Make DOL failed"), false);
    return false;
  }

  const QString out_dir = m_project->GetFolderPath("output");
  QDir().mkpath(out_dir);
  const QString dol_path =
      QDir(out_dir).filePath(QFileInfo(elf_path).completeBaseName() + QStringLiteral(".dol"));
  QFile out(dol_path);
  if (!out.open(QFile::WriteOnly | QFile::Truncate) ||
      out.write(reinterpret_cast<const char*>(dol->data()), qint64(dol->size())) !=
          qint64(dol->size()))
  {
    emit OutputLine(tr("[LINK] Couldn't write %1").arg(QDir::toNativeSeparators(dol_path)), true);
    emit StatusMessage(tr("Make DOL failed"), false);
    return false;
  }

  emit OutputLine(tr("[LINK] %1: %2 text and %3 data sections, BSS %4 bytes at %5, entry %6")
                      .arg(elf_name)
                      .arg(info.text_sections)
                      .arg(info.data_sections)
                      .arg(QLocale().toString(qint64(info.bss_size)))
                      .arg(info.bss_address, 8, 16, QLatin1Char('0'))
                      .arg(info.entry, 8, 16, QLatin1Char('0')),
                  false);
  const QString summary = tr("Generated: %1 (%2)")
                              .arg(QFileInfo(dol_path).fileName(), Bytes(qint64(dol->size())));
  emit StatusMessage(QStringLiteral("[LINK] ") + summary, true);
  return true;
}

bool DevkitPackager::PackGcm()
{
  if (!m_project->IsOpen() || m_packing)
    return false;

  const QString disc = m_project->GetFolderPath("disc");
  const QString main_dol = QDir(disc).filePath(QStringLiteral("sys/main.dol"));

  // The disc boots the project's DOL: put a newer one from Make DOL in place first.
  const QString target = m_project->ResolveRunTarget();
  const QString elf = FindElf();
  const QString made_dol =
      target.endsWith(QStringLiteral(".dol"), Qt::CaseInsensitive) ?
          target :
          (elf.isEmpty() ? QString() :
                           QDir(m_project->GetFolderPath("output"))
                               .filePath(QFileInfo(elf).completeBaseName() + QStringLiteral(".dol")));
  if (QDir(disc).exists(QStringLiteral("sys")) && QFileInfo::exists(made_dol) &&
      QFileInfo(made_dol) != QFileInfo(main_dol) &&
      (!QFileInfo::exists(main_dol) ||
       QFileInfo(made_dol).lastModified() > QFileInfo(main_dol).lastModified()))
  {
    QFile::remove(main_dol);
    if (!QFile::copy(made_dol, main_dol))
    {
      emit OutputLine(tr("[PACK] Couldn't copy %1 to %2")
                          .arg(QDir::toNativeSeparators(made_dol),
                               QDir::toNativeSeparators(main_dol)),
                      true);
      emit StatusMessage(tr("Pack GCM failed"), false);
      return false;
    }
    emit OutputLine(tr("[PACK] Copied %1 to %2")
                        .arg(QFileInfo(made_dol).fileName(),
                             QDir(m_project->GetPath()).relativeFilePath(main_dol)),
                    false);
  }

  if (!QFileInfo::exists(main_dol))
  {
    emit OutputLine(tr("[PACK] %1 is missing. Pack GCM packs the project's disc folder (sys/ and "
                       "files/); set its location under Project Folders in the Project Window.")
                        .arg(QDir::toNativeSeparators(main_dol)),
                    true);
    emit StatusMessage(tr("Pack GCM failed: no disc folder"), false);
    return false;
  }

  std::unique_ptr<DiscIO::BlobReader> reader =
      DiscIO::DirectoryBlobReader::Create(main_dol.toStdString());
  if (!reader)
  {
    emit OutputLine(tr("[PACK] %1 isn't a complete disc folder: sys/ needs boot.bin, bi2.bin, "
                       "apploader.img and main.dol.")
                        .arg(QDir::toNativeSeparators(disc)),
                    true);
    emit StatusMessage(tr("Pack GCM failed"), false);
    return false;
  }

  const QString out_dir = m_project->GetFolderPath("output");
  QDir().mkpath(out_dir);
  const QString gcm = QDir(out_dir).filePath(m_project->GetName() + QStringLiteral(".gcm"));

  if (m_thread.joinable())
    m_thread.join();
  m_cancel = false;
  m_packing = true;
  emit PackingChanged(true);
  emit OutputLine(tr("[PACK] Packing %1 into %2...")
                      .arg(QDir::toNativeSeparators(disc), QFileInfo(gcm).fileName()),
                  false);

  m_thread = std::thread([this, reader = std::move(reader), main_dol, gcm]() mutable {
    QElapsedTimer last;
    last.start();
    const bool ok = DiscIO::ConvertToPlain(
        reader.get(), main_dol.toStdString(), gcm.toStdString(),
        [this, &last](const std::string&, float percent) {
          if (last.elapsed() >= 250)
          {
            last.restart();
            const QString text = tr("Packing GCM... %1%").arg(int(percent * 100));
            QMetaObject::invokeMethod(this, [this, text] { emit Progress(text); },
                                      Qt::QueuedConnection);
          }
          return !m_cancel.load();
        });
    const QString error = m_cancel ? tr("cancelled") : tr("couldn't write the image");
    QMetaObject::invokeMethod(
        this, [this, ok, gcm, error] { FinishPack(ok, gcm, error); }, Qt::QueuedConnection);
  });
  return true;
}

void DevkitPackager::CancelPack()
{
  m_cancel = true;
}

void DevkitPackager::FinishPack(bool success, const QString& output, const QString& error)
{
  if (m_thread.joinable())
    m_thread.join();
  m_packing = false;
  emit PackingChanged(false);
  if (!success)
  {
    QFile::remove(output);
    emit OutputLine(tr("[PACK] Pack GCM failed: %1").arg(error), true);
    emit StatusMessage(tr("Pack GCM failed"), false);
    return;
  }
  const QString summary =
      tr("Created: %1 (%2)").arg(QFileInfo(output).fileName(), Bytes(QFileInfo(output).size()));
  emit StatusMessage(QStringLiteral("[PACK] ") + summary, true);
}
