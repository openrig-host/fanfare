#pragma once

#include <JuceHeader.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>
#include "../FmCore4.h"
#include "FmUiMetrics.h"

namespace FanfareFm::Ui {

struct FmAlgGraph {
  struct Edge {
    int srcOp;
    int dstOp;
  };

  int numOps = 6;
  int maxRow = 0;
  int fbInOp = -1;
  int fbOutOp = -1;
  std::array<bool, 6> active{};
  std::array<bool, 6> isCarrier{};
  std::array<int, 6> row{};
  std::array<float, 6> colX{};
  std::vector<Edge> edges;

  static FmAlgGraph build(int algIndex, int nOps) {
    FmAlgGraph g;
    g.numOps = (nOps == 4) ? 4 : 6;
    const int clampedAlg =
        (g.numOps == 6) ? juce::jlimit(0, 31, algIndex)
                        : juce::jlimit(0, FmCore4::kNumAlgorithms - 1, algIndex);

    int flags[6] = {0, 0, 0, 0, 0, 0};
    for (int op = 0; op < g.numOps; ++op) {
      flags[op] = (g.numOps == 6) ? FmAlgAccess::opFlags6(clampedAlg, op)
                                  : FmCore4::kAlgorithms[clampedAlg][op];
    }

    std::vector<int> busOps[3];
    std::vector<int> rawTargets[6];
    std::vector<Edge> rawEdges;

    for (int op = 0; op < g.numOps; ++op) {
      const int f = flags[op];
      if (f == 0)
        continue;

      const int inbus = (f >> 4) & 3;
      const int outbus = f & 3;
      const bool add = (f & OUT_BUS_ADD) != 0;

      if ((f & FB_IN) != 0)
        g.fbInOp = op;
      if ((f & FB_OUT) != 0)
        g.fbOutOp = op;

      if (inbus > 0 && !busOps[inbus].empty()) {
        for (int src : busOps[inbus]) {
          rawEdges.push_back({src, op});
          rawTargets[src].push_back(op);
        }
      }

      if (outbus == 0) {
        if (add)
          g.isCarrier[(size_t)op] = true;
      } else {
        if (add)
          busOps[outbus].push_back(op);
        else
          busOps[outbus] = {op};
      }
    }

    for (int op = g.numOps - 1; op >= 0; --op) {
      if (g.isCarrier[(size_t)op]) {
        g.active[(size_t)op] = true;
        g.row[(size_t)op] = 0;
      } else {
        int bestTargetRow = -1;
        for (int t : rawTargets[op]) {
          if (g.active[(size_t)t])
            bestTargetRow = juce::jmax(bestTargetRow, g.row[(size_t)t]);
        }
        if (bestTargetRow >= 0) {
          g.active[(size_t)op] = true;
          g.row[(size_t)op] = bestTargetRow + 1;
          g.maxRow = juce::jmax(g.maxRow, g.row[(size_t)op]);
        }
      }
    }

    for (const auto &e : rawEdges) {
      if (g.active[(size_t)e.srcOp] && g.active[(size_t)e.dstOp])
        g.edges.push_back(e);
    }

    if (g.fbInOp >= 0 && !g.active[(size_t)g.fbInOp])
      g.fbInOp = -1;
    if (g.fbOutOp >= 0 && !g.active[(size_t)g.fbOutOp])
      g.fbOutOp = -1;

    int carrierCol = 0;
    for (int op = g.numOps - 1; op >= 0; --op) {
      if (g.active[(size_t)op] && g.row[(size_t)op] == 0)
        g.colX[(size_t)op] = static_cast<float>(carrierCol++);
    }

    for (int r = 1; r <= g.maxRow; ++r) {
      struct RowNode {
        int op;
        int dispNum;
        float wantX;
      };
      std::vector<RowNode> rowNodes;
      for (int op = g.numOps - 1; op >= 0; --op) {
        if (!g.active[(size_t)op] || g.row[(size_t)op] != r)
          continue;
        float sumX = 0.0f;
        int cnt = 0;
        for (const auto &e : g.edges) {
          if (e.srcOp == op) {
            sumX += g.colX[(size_t)e.dstOp];
            ++cnt;
          }
        }
        const float wx = (cnt > 0) ? (sumX / static_cast<float>(cnt)) : 0.0f;
        rowNodes.push_back({op, g.numOps - op, wx});
      }

      std::sort(rowNodes.begin(), rowNodes.end(),
                [](const RowNode &a, const RowNode &b) {
                  if (std::abs(a.wantX - b.wantX) > 0.05f)
                    return a.wantX < b.wantX;
                  return a.dispNum < b.dispNum;
                });

      size_t idx = 0;
      while (idx < rowNodes.size()) {
        size_t end = idx + 1;
        while (end < rowNodes.size() &&
               std::abs(rowNodes[end].wantX - rowNodes[idx].wantX) <= 0.05f) {
          ++end;
        }
        const float center = rowNodes[idx].wantX;
        const int groupSize = static_cast<int>(end - idx);
        for (int k = 0; k < groupSize; ++k) {
          const float offset =
              static_cast<float>(k) - 0.5f * static_cast<float>(groupSize - 1);
          g.colX[(size_t)rowNodes[idx + (size_t)k].op] = center + offset;
        }
        idx = end;
      }

      for (size_t k = 1; k < rowNodes.size(); ++k) {
        const int prevOp = rowNodes[k - 1].op;
        const int curOp = rowNodes[k].op;
        if (g.colX[(size_t)curOp] < g.colX[(size_t)prevOp] + 1.0f)
          g.colX[(size_t)curOp] = g.colX[(size_t)prevOp] + 1.0f;
      }
    }

    return g;
  }

  juce::String describeSummary() const {
    int carriers = 0;
    for (int op = 0; op < numOps; ++op)
      if (active[(size_t)op] && isCarrier[(size_t)op])
        ++carriers;
    return juce::String(carriers) + (carriers == 1 ? " Carrier" : " Carriers") +
           "  |  Depth " + juce::String(maxRow + 1);
  }
};

class FmAlgorithmView : public juce::Component,
                        public juce::SettableTooltipClient {
public:
  FmAlgorithmView() {
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
    setTooltip("FM Algorithm routing (Carriers in gold, Modulators in cyan) - "
               "Click to select from list, or scroll wheel to step");
  }

  std::function<void(int)> onAlgorithmSelected;
  std::function<void(int)> onStepAlgorithm;

  void setState(int algIdx, int maxAlgs, int nOps, uint8_t opEnableMask) {
    const int clampedN = (nOps == 4) ? 4 : 6;
    const int clampedMax = juce::jmax(1, maxAlgs);
    const int clampedAlg = juce::jlimit(0, clampedMax - 1, algIdx);
    if (algIndex == clampedAlg && maxAlgCount == clampedMax &&
        numOps == clampedN && opMask == opEnableMask && graphValid) {
      return;
    }
    algIndex = clampedAlg;
    maxAlgCount = clampedMax;
    numOps = clampedN;
    opMask = opEnableMask;
    graph = FmAlgGraph::build(algIndex, numOps);
    graphValid = true;
    repaint();
  }

  void paint(juce::Graphics &g) override {
    const auto outer = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(ThemeManager::get(Theme::Role::sunken));
    g.fillRoundedRectangle(outer, 3.0f);
    g.setColour(ThemeManager::get(hovered ? Theme::Role::accent
                                          : Theme::Role::border));
    g.drawRoundedRectangle(outer, 3.0f, 1.0f);

    if (!graphValid)
      graph = FmAlgGraph::build(algIndex, numOps);

    auto pad = outer.reduced(7.0f, 6.0f);
    const float busY = pad.getBottom() - 2.0f;
    pad.removeFromBottom(8.0f);
    if (graph.fbInOp >= 0 && graph.fbOutOp >= 0)
      pad.removeFromRight(7.0f);

    float minX = 999.0f, maxX = -999.0f;
    for (int op = 0; op < graph.numOps; ++op) {
      if (!graph.active[(size_t)op])
        continue;
      minX = juce::jmin(minX, graph.colX[(size_t)op]);
      maxX = juce::jmax(maxX, graph.colX[(size_t)op]);
    }
    if (minX > maxX)
      return;

    const float colsSpan = juce::jmax(1.0f, maxX - minX + 1.0f);
    const float rowsSpan = static_cast<float>(graph.maxRow + 1);

    const float cellW = pad.getWidth() / colsSpan;
    const float cellH = pad.getHeight() / rowsSpan;
    const float boxW = juce::jlimit(18.0f, 28.0f, cellW - 5.0f);
    const float boxH = juce::jlimit(13.0f, 20.0f, cellH - 6.0f);

    std::array<juce::Rectangle<float>, 6> opBoxes{};
    for (int op = 0; op < graph.numOps; ++op) {
      if (!graph.active[(size_t)op])
        continue;
      const float normCol = (graph.colX[(size_t)op] - minX + 0.5f) / colsSpan;
      const float cx = pad.getX() + normCol * pad.getWidth();
      const float invRow =
          static_cast<float>(graph.maxRow - graph.row[(size_t)op]) + 0.5f;
      const float cy = pad.getY() + (invRow / rowsSpan) * pad.getHeight();
      opBoxes[(size_t)op] =
          juce::Rectangle<float>(cx - boxW * 0.5f, cy - boxH * 0.5f, boxW, boxH);
    }

    const juce::Colour wireCol =
        ThemeManager::get(Theme::Role::textDim).withAlpha(0.85f);
    g.setColour(wireCol);

    for (const auto &e : graph.edges) {
      const auto &src = opBoxes[(size_t)e.srcOp];
      const auto &dst = opBoxes[(size_t)e.dstOp];
      const float x1 = src.getCentreX();
      const float y1 = src.getBottom();
      const float x2 = dst.getCentreX();
      const float y2 = dst.getY();
      const float midY = 0.5f * (y1 + y2);

      juce::Path p;
      p.startNewSubPath(x1, y1);
      if (std::abs(x1 - x2) < 1.2f) {
        p.lineTo(x2, y2 - 2.0f);
      } else {
        p.lineTo(x1, midY);
        p.lineTo(x2, midY);
        p.lineTo(x2, y2 - 2.0f);
      }
      g.strokePath(p, juce::PathStrokeType(1.35f));

      juce::Path head;
      head.addTriangle(x2 - 2.6f, y2 - 3.6f, x2 + 2.6f, y2 - 3.6f, x2, y2);
      g.fillPath(head);
    }

    float minCarrierX = 9999.0f, maxCarrierX = -9999.0f;
    const juce::Colour carrierCol = ThemeManager::get(Theme::Role::foh);
    const juce::Colour modCol = ThemeManager::get(Theme::Role::iem);

    for (int op = 0; op < graph.numOps; ++op) {
      if (!graph.active[(size_t)op] || !graph.isCarrier[(size_t)op])
        continue;
      const auto &box = opBoxes[(size_t)op];
      const float cx = box.getCentreX();
      minCarrierX = juce::jmin(minCarrierX, cx);
      maxCarrierX = juce::jmax(maxCarrierX, cx);
      g.setColour(carrierCol.withAlpha(0.75f));
      g.drawLine(cx, box.getBottom(), cx, busY, 1.4f);
    }

    if (minCarrierX <= maxCarrierX) {
      g.setColour(carrierCol.withAlpha(0.85f));
      const float leftX = juce::jmin(minCarrierX, outer.getCentreX() - 10.0f);
      const float rightX = juce::jmax(maxCarrierX, outer.getCentreX() + 10.0f);
      g.drawLine(leftX, busY, rightX, busY, 1.8f);
      g.fillEllipse(outer.getCentreX() - 2.2f, busY - 2.2f, 4.4f, 4.4f);
    }

    if (graph.fbInOp >= 0 && graph.fbOutOp >= 0) {
      const auto &inBox = opBoxes[(size_t)graph.fbInOp];
      const auto &outBox = opBoxes[(size_t)graph.fbOutOp];
      const float loopRight = juce::jmax(inBox.getRight(), outBox.getRight()) + 5.5f;
      const float loopTop = inBox.getY() - 4.2f;
      const float entryX = inBox.getRight() - boxW * 0.25f;

      juce::Path fb;
      fb.startNewSubPath(outBox.getRight(), outBox.getCentreY());
      fb.lineTo(loopRight, outBox.getCentreY());
      fb.lineTo(loopRight, loopTop);
      fb.lineTo(entryX, loopTop);
      fb.lineTo(entryX, inBox.getY());

      g.setColour(ThemeManager::get(Theme::Role::warn));
      g.strokePath(fb, juce::PathStrokeType(1.3f, juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
      juce::Path fbArrow;
      fbArrow.addTriangle(entryX - 2.3f, inBox.getY() - 3.0f, entryX + 2.3f,
                          inBox.getY() - 3.0f, entryX, inBox.getY() + 0.4f);
      g.fillPath(fbArrow);
    }

    for (int op = 0; op < graph.numOps; ++op) {
      if (!graph.active[(size_t)op])
        continue;
      const auto &box = opBoxes[(size_t)op];
      const bool enabled = ((opMask >> op) & 1u) != 0;
      const bool carrier = graph.isCarrier[(size_t)op];
      const juce::Colour roleCol = carrier ? carrierCol : modCol;

      g.setColour(enabled ? roleCol.withAlpha(0.24f)
                          : ThemeManager::get(Theme::Role::panel));
      g.fillRoundedRectangle(box, 2.5f);

      g.setColour(enabled ? roleCol
                          : ThemeManager::get(Theme::Role::borderStrong)
                                .withAlpha(0.45f));
      g.drawRoundedRectangle(box, 2.5f, enabled ? 1.35f : 0.9f);

      g.setColour(enabled ? ThemeManager::get(Theme::Role::text)
                          : ThemeManager::get(Theme::Role::textFaint));
      g.setFont(BoutiqueLookAndFeel::monoFont(10.0f, true));
      g.drawText(juce::String(graph.numOps - op), box.toNearestInt(),
                 juce::Justification::centred, false);
    }
  }

  void mouseEnter(const juce::MouseEvent &) override {
    hovered = true;
    repaint();
  }
  void mouseExit(const juce::MouseEvent &) override {
    hovered = false;
    repaint();
  }

  void mouseDown(const juce::MouseEvent &) override {
    juce::PopupMenu m;
    m.addSectionHeader((numOps == 6 ? "6-Operator Algorithms (1-" : "4-Operator Algorithms (1-") +
                       juce::String(maxAlgCount) + ")");
    for (int a = 0; a < maxAlgCount; ++a) {
      const auto info = FmAlgGraph::build(a, numOps);
      const juce::String label =
          "ALG " + juce::String(a + 1).paddedLeft('0', 2) + "   [" +
          info.describeSummary() + "]";
      m.addItem(a + 1, label, true, a == algIndex);
    }
    juce::Component::SafePointer<FmAlgorithmView> safeThis(this);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                    [safeThis](int res) {
                      if (safeThis != nullptr && res > 0 &&
                          safeThis->onAlgorithmSelected) {
                        safeThis->onAlgorithmSelected(res - 1);
                      }
                    });
  }

  void mouseWheelMove(const juce::MouseEvent &,
                      const juce::MouseWheelDetails &wheel) override {
    if (std::abs(wheel.deltaY) <= 1e-5f || !onStepAlgorithm)
      return;
    onStepAlgorithm(wheel.deltaY > 0.0f ? 1 : -1);
  }

private:
  int algIndex = 0;
  int maxAlgCount = 32;
  int numOps = 6;
  uint8_t opMask = 63;
  FmAlgGraph graph;
  bool graphValid = false;
  bool hovered = false;
};

} // namespace FanfareFm::Ui
