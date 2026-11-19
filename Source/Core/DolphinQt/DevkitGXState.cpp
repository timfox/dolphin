// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/DevkitGXState.h"

#include <cstring>
#include <memory>
#include <set>
#include <string>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <fmt/format.h>

#include "Core/Core.h"
#include "Core/System.h"
#include "DolphinQt/IconSet.h"
#include "DolphinQt/Settings.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/TextureDecoder.h"

namespace
{
QTreeWidgetItem* AddItem(QTreeWidgetItem* parent, const QString& name, const std::string& value)
{
  // Multi-line descriptions (the register formatters write one field per line) become children.
  QStringList lines = QString::fromStdString(value).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  // A leading line without a "field: value" form (the combiners' equation) is the summary.
  QString summary;
  if (lines.size() == 1 || (!lines.isEmpty() && !lines[0].contains(QStringLiteral(": "))))
    summary = lines.takeFirst();
  auto* const item = new QTreeWidgetItem(parent, {name, summary});
  if (!lines.isEmpty())
  {
    for (const QString& line : lines)
    {
      const qsizetype colon = line.indexOf(QLatin1Char(':'));
      if (colon > 0)
        new QTreeWidgetItem(item, {line.left(colon).trimmed(), line.mid(colon + 1).trimmed()});
      else
        new QTreeWidgetItem(item, {QString(), line.trimmed()});
    }
  }
  return item;
}

QTreeWidgetItem* AddSection(QTreeWidget* tree, const QString& name, const QString& summary,
                            std::string_view icon)
{
  auto* const item = new QTreeWidgetItem(tree, {name, summary});
  item->setIcon(0, IconSet::Get(icon));
  QFont font = item->font(0);
  font.setBold(true);
  item->setFont(0, font);
  item->setExpanded(true);
  return item;
}
}  // namespace

GXStateWidget::GXStateWidget(QWidget* parent) : QDockWidget(parent)
{
  setWindowTitle(tr("GX State"));
  setObjectName(QStringLiteral("gxstate"));
  setAllowedAreas(Qt::AllDockWidgetAreas);

  auto* const refresh = new QPushButton(IconSet::Get("arrows-rotate"), tr("Refresh"));
  m_live = new QCheckBox(tr("Live"));
  m_live->setToolTip(tr("Refresh once a second while the game runs. The state is read between "
                        "draws, so it can be from any point in the frame."));
  m_status = new QLabel;
  m_status->setObjectName(QStringLiteral("projectHint"));

  m_tree = new QTreeWidget;
  m_tree->setColumnCount(2);
  m_tree->setHeaderLabels({tr("State"), tr("Value")});
  m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_tree->setUniformRowHeights(true);
  m_tree->setAlternatingRowColors(true);

  auto* const bar = new QHBoxLayout;
  bar->addWidget(refresh);
  bar->addWidget(m_live);
  bar->addWidget(m_status, 1);

  auto* const widget = new QWidget;
  auto* const layout = new QVBoxLayout(widget);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->addLayout(bar);
  layout->addWidget(m_tree);
  setWidget(widget);

  m_timer = new QTimer(this);
  m_timer->setInterval(1000);
  connect(m_timer, &QTimer::timeout, this, &GXStateWidget::Refresh);
  connect(refresh, &QPushButton::clicked, this, &GXStateWidget::Refresh);
  connect(m_live, &QCheckBox::toggled, this, [this] {
    OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
  });
  connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
          &GXStateWidget::OnEmulationStateChanged);
  OnEmulationStateChanged(Core::GetState(Core::System::GetInstance()));
}

void GXStateWidget::OnEmulationStateChanged(Core::State state)
{
  if (state == Core::State::Running && m_live->isChecked())
    m_timer->start();
  else
    m_timer->stop();
  // A pause is the moment to look at the state: it stays put until emulation resumes.
  if (state == Core::State::Paused || state == Core::State::Uninitialized)
    Refresh();
}

void GXStateWidget::Refresh()
{
  const Core::State state = Core::GetState(Core::System::GetInstance());
  m_tree->clear();
  if (state == Core::State::Uninitialized || state == Core::State::Starting)
  {
    m_status->setText(tr("Start a game to inspect its GX state."));
    return;
  }

  // Copy the registers first, so the view is one consistent read.
  const auto bp = std::make_unique<BPMemory>();
  std::memcpy(static_cast<void*>(bp.get()), &bpmem, sizeof(BPMemory));
  m_status->setText(state == Core::State::Paused ? tr("Paused: state at the last draw") :
                                                   tr("Running: state between draws"));

  const GenMode& gen = bp->genMode;
  const u32 stages = gen.numtevstages + 1;
  const u32 ind_stages = gen.numindstages;

  // Pipeline summary.
  QTreeWidgetItem* const summary = AddSection(
      m_tree, tr("Pipeline"),
      tr("%1 TEV stage(s), %2 texgen(s), %3 color channel(s)")
          .arg(stages)
          .arg(gen.numtexgens.Value())
          .arg(gen.numcolchans.Value()),
      "layer-group");
  AddItem(summary, tr("Indirect stages"), fmt::to_string(ind_stages));
  AddItem(summary, tr("Cull mode"), fmt::to_string(gen.cull_mode));
  AddItem(summary, tr("Multisampling"), gen.multisampling ? "On" : "Off");
  AddItem(summary, tr("Scissor top-left"), fmt::to_string(bp->scissorTL));
  AddItem(summary, tr("Scissor bottom-right"), fmt::to_string(bp->scissorBR));

  // TEV stages.
  std::set<u32> texmaps;
  QTreeWidgetItem* const tev =
      AddSection(m_tree, tr("TEV Stages"), tr("%1 active").arg(stages), "palette");
  for (u32 i = 0; i < stages; ++i)
  {
    const TwoTevStageOrders& order = bp->tevorders[i / 2];
    const int odd = i & 1;
    const bool textured = order.getEnable(odd);
    const QString inputs =
        textured ? tr("TEXMAP%1 via TEXCOORD%2, %3")
                       .arg(order.getTexMap(odd))
                       .arg(order.getTexCoord(odd))
                       .arg(QString::fromStdString(fmt::to_string(order.getColorChan(odd)))) :
                   tr("no texture, %1")
                       .arg(QString::fromStdString(fmt::to_string(order.getColorChan(odd))));
    if (textured)
      texmaps.insert(order.getTexMap(odd));

    auto* const stage = new QTreeWidgetItem(tev, {tr("Stage %1").arg(i), inputs});
    const TevStageCombiner& combiner = bp->combiners[i];
    AddItem(stage, tr("Color"), fmt::to_string(combiner.colorC));
    AddItem(stage, tr("Alpha"), fmt::to_string(combiner.alphaC));
    AddItem(stage, tr("Konst color"), fmt::to_string(bp->tevksel.GetKonstColor(i)));
    AddItem(stage, tr("Konst alpha"), fmt::to_string(bp->tevksel.GetKonstAlpha(i)));
    if (ind_stages > 0 && bp->tevind[i].hex != 0)
      AddItem(stage, tr("Indirect"), fmt::to_string(bp->tevind[i]));
    stage->setExpanded(i == 0);
  }

  // TEV registers.
  QTreeWidgetItem* const regs = AddSection(m_tree, tr("TEV Registers"), QString(), "microchip");
  for (u32 i = 0; i < 4; ++i)
    AddItem(regs, tr("Register %1").arg(i), fmt::to_string(bp->tevregs[i]));
  regs->setExpanded(false);

  // Textures the stages sample.
  QTreeWidgetItem* const textures =
      AddSection(m_tree, tr("Textures"), tr("%1 sampled").arg(texmaps.size()), "image");
  for (const u32 map : texmaps)
  {
    const TexUnit& unit = bp->tex.GetUnit(map);
    const QString size = QStringLiteral("%1×%2 %3 at 0x%4")
                             .arg(unit.texImage0.width + 1)
                             .arg(unit.texImage0.height + 1)
                             .arg(QString::fromStdString(fmt::to_string(unit.texImage0.format)))
                             .arg(unit.texImage3.image_base << 5, 8, 16, QLatin1Char('0'));
    auto* const item = new QTreeWidgetItem(textures, {tr("TEXMAP%1").arg(map), size});
    AddItem(item, tr("Sampling"), fmt::to_string(unit.texMode0));
    AddItem(item, tr("LOD"), fmt::to_string(unit.texMode1));
  }

  // Pixel engine.
  QTreeWidgetItem* const pe = AddSection(m_tree, tr("Pixel Engine"), QString(), "display");
  AddItem(pe, tr("Depth"), fmt::to_string(bp->zmode));
  AddItem(pe, tr("Blending"), fmt::to_string(bp->blendmode));
  AddItem(pe, tr("Alpha test"), fmt::to_string(bp->alpha_test));
  AddItem(pe, tr("Destination alpha"), fmt::to_string(bp->dstalpha));
  AddItem(pe, tr("Control"), fmt::to_string(bp->zcontrol));
  AddItem(pe, tr("Fog"), fmt::to_string(bp->fog.c_proj_fsel));
}
