#include "LibraryPanel.h"
#include "BoutiqueLookAndFeel.h"
#include "ButtonRoles.h"
#include "FanfareConstants.h"
#include "SetupMidiTriggers.h"
#include "SetlistManager.h"
#include "ThemeManager.h"
#include "VersionHistoryModal.h"

LibraryList::LibraryList(Kind k) : kind(k) {
  setModel(this);
  setRowHeight(24);
  setMultipleSelectionEnabled(false);
  setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
  loadFavorites();
}

void LibraryList::loadFavorites() {
  favorites.clear();
  auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Fanfare");
  auto favFile = appData.getChildFile("library_favorites.txt");
  if (favFile.existsAsFile()) {
    juce::StringArray lines;
    favFile.readLines(lines);
    for (const auto &l : lines) {
      if (l.trim().isNotEmpty())
        favorites.insert(l.trim());
    }
  }
}

void LibraryList::saveFavorites() {
  auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Fanfare");
  if (!appData.exists()) appData.createDirectory();
  auto favFile = appData.getChildFile("library_favorites.txt");
  juce::String text;
  for (const auto &path : favorites)
    text += path + "\n";
  favFile.replaceWithText(text);
}

void LibraryList::toggleFavorite(const juce::File &f) {
  juce::String path = f.getFullPathName();
  if (favorites.count(path) > 0)
    favorites.erase(path);
  else
    favorites.insert(path);
  saveFavorites();
  refresh();
}

juce::Colour LibraryList::detectBadgeColour(const juce::String &name, Kind k) {
  // Muted stage-tape colors matching HUD sound-tag palette
  juce::String n = name.toLowerCase();
  if (n.contains("piano") || n.contains("wurl") || n.contains("rhodes") || n.contains("grand") || n.contains("steinway") || n.contains("keys"))
    return juce::Colour(0xFF3D8FB8); // Stage Blue
  if (n.contains("organ") || n.contains("b3") || n.contains("leslie"))
    return juce::Colour(0xFFC98A2E); // Warm Amber
  if (n.contains("brass") || n.contains("horn") || n.contains("sax"))
    return juce::Colour(0xFFB8564F); // Coral Red
  if (n.contains("synth") || n.contains("lead") || n.contains("pad") || n.contains("juno") || n.contains("arp") || n.contains("square"))
    return juce::Colour(0xFF8668B0); // Muted Violet
  if (n.contains("sample") || n.contains("perc") || n.contains("marimba") || n.contains("drum") || n.contains("handclap"))
    return juce::Colour(0xFF4A9A68); // Sage Green
  if (k == Kind::Setups)
    return ThemeManager::get(Theme::Role::accent);
  return ThemeManager::get(Theme::Role::engrave);
}

void LibraryList::setDirectory(const juce::File &dir, const juce::String &wildcard) {
  directory = dir;
  wc = wildcard;
  refresh();
}

void LibraryList::setFilterQuery(const juce::String &query) {
  filterQuery = query.trim().toLowerCase();
  refresh();
}

void LibraryList::toggleFolderCollapsed(const juce::String &cat) {
  if (collapsedFolders.count(cat) > 0)
    collapsedFolders.erase(cat);
  else
    collapsedFolders.insert(cat);
  refresh();
}

void LibraryList::createNewFolder() {
  auto *aw = new juce::AlertWindow("New Category Folder",
                                   "Enter a folder name to group songs/strips:",
                                   juce::MessageBoxIconType::InfoIcon);
  aw->addTextEditor("folderName", "", "Folder Name:");
  aw->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
  aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

  juce::Component::SafePointer<LibraryList> safeThis(this);
  juce::Component::SafePointer<juce::AlertWindow> safeAw(aw);
  aw->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, safeAw](int result) {
    if (result == 1 && safeThis != nullptr && safeAw != nullptr) {
      juce::String text = safeAw->getTextEditorContents("folderName").trim();
      if (text.isNotEmpty()) {
        juce::String cleanName = juce::File::createLegalFileName(text);
        juce::File newDir = safeThis->directory.getChildFile(cleanName);
        if (!newDir.exists())
          newDir.createDirectory();
        safeThis->refresh();
      }
    }
  }), true);
}

void LibraryList::moveItemToFolder(const juce::File &file, const juce::String &targetSubfolder) {
  if (!file.existsAsFile())
    return;
  juce::File targetDir = (targetSubfolder.isEmpty() || targetSubfolder.equalsIgnoreCase("GENERAL"))
                             ? directory
                             : directory.getChildFile(targetSubfolder);
  if (!targetDir.exists())
    targetDir.createDirectory();

  juce::File destFile = targetDir.getChildFile(file.getFileName());
  if (file != destFile) {
    file.moveFileTo(destFile);
    refresh();
  }
}

void LibraryList::refresh() {
  directory.createDirectory();
  allFiles.clear();
  items.clear();

  if (directory.isDirectory()) {
    juce::Array<juce::File> found;
    directory.findChildFiles(found, juce::File::findFiles, true, wc);
    if (kind == Kind::Setups) {
      juce::Array<juce::File> more;
      directory.findChildFiles(more, juce::File::findFiles, true, "*.orsong");
      for (auto &f : more)
        found.addIfNotAlreadyThere(f);
    }
    for (auto &f : found)
      allFiles.push_back(f);

    std::sort(allFiles.begin(), allFiles.end(),
              [](const juce::File &a, const juce::File &b) {
                return a.getFileName().compareIgnoreCase(b.getFileName()) < 0;
              });

    std::vector<Item> favItems;
    std::map<juce::String, std::vector<Item>> categoryMap;

    juce::Array<juce::File> subDirs;
    directory.findChildFiles(subDirs, juce::File::findDirectories, false);
    for (const auto &sd : subDirs) {
      juce::String folderCat = sd.getFileName().toUpperCase();
      categoryMap[folderCat] = {};
    }

    for (const auto &f : allFiles) {
      juce::String fileName = f.getFileNameWithoutExtension();
      juce::String fullPath = f.getFullPathName();
      juce::String relPath = f.getRelativePathFrom(directory);

      if (filterQuery.isNotEmpty()) {
        if (!fileName.toLowerCase().contains(filterQuery) && !relPath.toLowerCase().contains(filterQuery))
          continue;
      }

      bool isFav = (favorites.count(fullPath) > 0);
      juce::Colour badge = detectBadgeColour(fileName, kind);

      juce::String catName = "GENERAL";
      if (f.getParentDirectory() != directory) {
        catName = f.getParentDirectory().getFileName().toUpperCase();
      }

      Item it;
      it.file = f;
      it.displayName = fileName;
      it.category = catName;
      it.isFolderHeader = false;
      it.isFavorite = isFav;
      it.badgeColour = badge;

      if (isFav) {
        favItems.push_back(it);
      } else {
        categoryMap[catName].push_back(it);
      }
    }

    // 1. Favorites at the top (zero emoji)
    if (!favItems.empty()) {
      Item header;
      header.displayName = "FAVORITES";
      header.category = "FAVORITES";
      header.isFolderHeader = true;
      items.push_back(header);
      for (const auto &it : favItems)
        items.push_back(it);
    }

    // 2. Folder Subcategories & Root files
    for (auto &kv : categoryMap) {
      if (kv.first != "GENERAL") {
        Item header;
        bool isCollapsed = (collapsedFolders.count(kv.first) > 0);
        header.displayName = kv.first;
        header.category = kv.first;
        header.isFolderHeader = true;
        header.isCollapsed = isCollapsed;
        header.folderDir = directory.getChildFile(kv.first);
        items.push_back(header);
        if (isCollapsed && filterQuery.isEmpty()) {
          continue;
        }
      }
      for (const auto &it : kv.second)
        items.push_back(it);
    }
  }

  updateContent();
  repaint();
}

int LibraryList::getNumRows() { return (int)items.size(); }

void LibraryList::paintListBoxItem(int row, juce::Graphics &g, int width,
                                   int height, bool isSelected) {
  if (row < 0 || row >= (int)items.size())
    return;

  const auto &t = ThemeManager::getInstance().active();
  const auto &item = items[row];

  // Folder Category Header Row
  if (item.isFolderHeader) {
    g.fillAll(t.sunken);
    const bool isFavHeader = (item.category == "FAVORITES");

    if (!isFavHeader) {
      juce::Path tri;
      float cx = 10.0f;
      float cy = height * 0.5f;
      if (item.isCollapsed) {
        tri.addTriangle(cx - 2.5f, cy - 3.5f, cx - 2.5f, cy + 3.5f, cx + 3.0f, cy);
      } else {
        tri.addTriangle(cx - 3.5f, cy - 2.0f, cx + 3.5f, cy - 2.0f, cx, cy + 3.0f);
      }
      g.setColour(t.engrave);
      g.fillPath(tri);
    } else {
      g.setColour(t.foh);
      g.fillRect(6.0f, 5.0f, 2.5f, (float)height - 10.0f);
    }

    g.setColour(isFavHeader ? t.foh : t.textDim);
    g.setFont(BoutiqueLookAndFeel::condensedFont(11.5f, 0.08f));
    g.drawText(item.displayName, isFavHeader ? 14 : 20, 0, width - 26, height,
               juce::Justification::centredLeft, true);

    g.setColour(t.hairline);
    g.drawHorizontalLine(height - 1, 0.0f, (float)width);
    return;
  }

  if (isSelected) {
    g.fillAll(t.accent.withAlpha(t.dark ? 0.22f : 0.16f));
    g.setColour(t.accent);
    g.fillRect(0.0f, 0.0f, 2.5f, (float)height);
  } else if (row % 2 == 1) {
    g.fillAll(t.text.withAlpha(0.02f));
  }

  // Stage-tape vertical stripe
  g.setColour(item.badgeColour.withAlpha(0.88f));
  g.fillRoundedRectangle(6.0f, 5.0f, 3.0f, (float)(height - 10), 1.0f);

  int textX = 15;
  if (item.isFavorite) {
    g.setColour(t.foh);
    g.fillEllipse((float)textX, height * 0.5f - 2.5f, 5.0f, 5.0f);
    textX += 10;
  }

  juce::File f = item.file;
  juce::String name = item.displayName;
  juce::Colour textCol = t.text;

  if (kind == Kind::Setups) {
    if (learningFile == f) {
      name += "  [LEARN]";
      textCol = t.warn;
    } else {
      auto trigger = SetupMidiTriggers::getInstance().getTrigger(f.getFileName());
      if (trigger.number != -1) {
        name += "  [" + trigger.toString() + "]";
        textCol = t.iem;
      }
    }
  }

  g.setColour(textCol);
  g.setFont(BoutiqueLookAndFeel::uiFont(13.0f, isSelected));
  g.drawText(name, textX, 0, width - textX - 8, height, juce::Justification::centredLeft, true);
}

void LibraryList::selectedRowsChanged(int) {}

void LibraryList::listBoxItemClicked(int row, const juce::MouseEvent &e) {
  selectedRow = row;
  if (row < 0 || row >= (int)items.size())
    return;

  const auto &item = items[row];

  if (item.isFolderHeader) {
    if (item.category == "FAVORITES")
      return;

    if (e.mods.isPopupMenu()) {
      juce::PopupMenu menu;
      menu.addItem(1, item.isCollapsed ? "Expand Folder" : "Collapse Folder");
      menu.addItem(2, "New Subfolder...");
      menu.addItem(3, "Reveal in Explorer");
      menu.addItem(4, "Delete Empty Folder");

      juce::Component::SafePointer<LibraryList> safeThis(this);
      const juce::String cat = item.category;
      const juce::File folderDir = item.folderDir;
      menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, cat, folderDir](int result) {
        if (safeThis == nullptr)
          return;
        if (result == 1) {
          safeThis->toggleFolderCollapsed(cat);
        } else if (result == 2) {
          safeThis->createNewFolder();
        } else if (result == 3) {
          if (folderDir.exists())
            folderDir.startAsProcess();
        } else if (result == 4) {
          if (folderDir.exists() && folderDir.findChildFiles(juce::File::findFiles, false).isEmpty()) {
            folderDir.deleteRecursively();
            safeThis->refresh();
          } else {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon, "Folder Not Empty",
                "Please delete or move all items out of this folder before deleting it.");
          }
        }
      });
    } else {
      toggleFolderCollapsed(item.category);
    }
    return;
  }

  if (e.mods.isPopupMenu()) {
    juce::PopupMenu menu;
    bool isFav = item.isFavorite;
    menu.addItem(10, isFav ? "Unpin Favorite" : "Pin to Favorites");
    menu.addSeparator();

    juce::PopupMenu moveSubmenu;
    juce::Array<juce::File> subDirs;
    directory.findChildFiles(subDirs, juce::File::findDirectories, false);

    moveSubmenu.addItem(100, "Root / General");
    int subId = 101;
    std::map<int, juce::String> folderIdMap;
    for (const auto &sd : subDirs) {
      juce::String folderName = sd.getFileName();
      moveSubmenu.addItem(subId, folderName);
      folderIdMap[subId] = folderName;
      subId++;
    }
    moveSubmenu.addSeparator();
    moveSubmenu.addItem(999, "+ New Folder...");
    menu.addSubMenu("Move to Folder", moveSubmenu);
    menu.addSeparator();

    if (kind == Kind::Setups) {
      menu.addItem(8, "Diagnose & repair (Rig Doctor)...");
      menu.addSeparator();
      bool isCurrentlyLearning = (learningFile == item.file);
      if (isCurrentlyLearning) {
        menu.addItem(3, "Cancel Learn");
      } else {
        menu.addItem(1, "Learn MIDI Trigger");
      }
      menu.addItem(2, "Clear MIDI Trigger");
      menu.addSeparator();
      menu.addItem(5, "Add to Setlist");
      menu.addSeparator();
    }

    menu.addItem(7, "Version history...");
    menu.addItem(6, "Reveal in Explorer");
    menu.addItem(4, "Delete Item");

    juce::File clickedFile = item.file;
    juce::Component::SafePointer<LibraryList> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis, clickedFile, folderIdMap](int result) {
      if (safeThis == nullptr || result == 0) return;
      auto file = clickedFile;

      if (result == 10) {
        safeThis->toggleFavorite(file);
      } else if (result == 100) {
        safeThis->moveItemToFolder(file, "");
      } else if (result == 999) {
        safeThis->createNewFolder();
      } else if (folderIdMap.count(result) > 0) {
        safeThis->moveItemToFolder(file, folderIdMap.at(result));
      } else if (result == 1) {
        safeThis->learningFile = file;
        safeThis->learningActive.store(true, std::memory_order_release);
        safeThis->repaint();
      } else if (result == 2) {
        SetupMidiTriggers::getInstance().clearTrigger(file.getFileName());
        safeThis->repaint();
      } else if (result == 3) {
        safeThis->learningFile = juce::File{};
        safeThis->learningActive.store(false, std::memory_order_release);
        safeThis->repaint();
      } else if (result == 4) {
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon, "Delete Item",
            "Are you sure you want to permanently delete '" + file.getFileNameWithoutExtension() + "'?",
            "Delete", "Cancel", nullptr,
            juce::ModalCallbackFunction::create([safeThis, file](int confirmResult) {
              if (confirmResult == 1) {
                file.deleteFile();
                if (safeThis != nullptr)
                  safeThis->refresh();
              }
            }));
      } else if (result == 5) {
        Fanfare::SetlistManager::getInstance().addSetup(file);
      } else if (result == 6) {
        file.revealToUser();
      } else if (result == 7) {
        Fanfare::showVersionHistoryDialog(file, [safeThis](const juce::File &restored) {
          if (safeThis == nullptr)
            return;
          safeThis->refresh();
          if (safeThis->kind != Kind::Strips && safeThis->onDoubleClicked)
            safeThis->onDoubleClicked(restored);
        });
      } else if (result == 8) {
        if (safeThis->onDiagnoseRequested)
          safeThis->onDiagnoseRequested(file);
      }
    });
  }
}

void LibraryList::listBoxItemDoubleClicked(int row, const juce::MouseEvent &) {
  if (row < 0 || row >= (int)items.size())
    return;
  const auto &item = items[row];
  if (item.isFolderHeader) {
    if (item.category != "FAVORITES")
      toggleFolderCollapsed(item.category);
    return;
  }

  if (kind != Kind::Strips && onDoubleClicked)
    onDoubleClicked(item.file);
}

juce::var LibraryList::getDragSourceDescription(const juce::SparseSet<int>& selectedRows) {
  if (selectedRows.size() > 0) {
    int row = selectedRows[0];
    if (row >= 0 && row < (int)items.size() && !items[row].isFolderHeader)
      return items[row].file.getFullPathName();
  }
  return {};
}

juce::File LibraryList::getSelectedFile() const {
  if (selectedRow >= 0 && selectedRow < (int)items.size() && !items[selectedRow].isFolderHeader)
    return items[selectedRow].file;
  return {};
}

LibraryPanel::LibraryPanel() {
  addAndMakeVisible(titleLabel);
  titleLabel.setFont(BoutiqueLookAndFeel::uiFont(14.0f, true));
  titleLabel.setJustificationType(juce::Justification::centredLeft);

  addAndMakeVisible(countLabel);
  countLabel.setFont(BoutiqueLookAndFeel::monoFont(11.0f));
  countLabel.setJustificationType(juce::Justification::centredRight);

  addAndMakeVisible(searchEditor);
  searchEditor.setFont(BoutiqueLookAndFeel::uiFont(13.0f, false));
  searchEditor.onTextChange = [this] {
    juce::String q = searchEditor.getText();
    if (setsList) setsList->setFilterQuery(q);
    if (setupsList) setupsList->setFilterQuery(q);
    if (stripsList) stripsList->setFilterQuery(q);
  };

  addAndMakeVisible(newFolderBtn);
  newFolderBtn.setButtonText("+ Folder");
  Fanfare::setButtonRole(newFolderBtn, Fanfare::ButtonRole::Subtle);
  newFolderBtn.setTooltip("Create a new category folder");
  newFolderBtn.onClick = [this] {
    if (context == Context::Songs && setupsList) setupsList->createNewFolder();
    else if (context == Context::Strips && stripsList) stripsList->createNewFolder();
    else if (context == Context::Queue && setsList) setsList->createNewFolder();
  };

  setupsList = std::make_unique<LibraryList>(LibraryList::Kind::Setups);
  setupsList->setDirectory(Fanfare::RigLibrary::getSongsDirectory(), "*.json");
  setupsList->onDoubleClicked = [this](const juce::File &f) {
    if (onSetupDoubleClicked)
      onSetupDoubleClicked(f);
  };
  setupsList->onDiagnoseRequested = [this](const juce::File &f) {
    if (onDiagnoseSetup)
      onDiagnoseSetup(f);
  };
  addChildComponent(setupsList.get());

  stripsList = std::make_unique<LibraryList>(LibraryList::Kind::Strips);
  stripsList->setDirectory(Fanfare::RigLibrary::getPresetsDirectory(), "*.orstrip");
  addChildComponent(stripsList.get());

  setlistPanel = std::make_unique<Fanfare::SetlistPanel>();
  addChildComponent(setlistPanel.get());

  // Queue context: live queue tiles on top, saved queue maps (.orset) below
  setsList = std::make_unique<LibraryList>(LibraryList::Kind::Sets);
  setsList->setDirectory(Fanfare::RigLibrary::getSetsDirectory(), "*.orset");
  setsList->onDoubleClicked = [this](const juce::File &f) {
    if (onSetDoubleClicked)
      onSetDoubleClicked(f);
  };
  mapsLabel.setText("Saved queue maps", juce::dontSendNotification);
  mapsLabel.setFont(BoutiqueLookAndFeel::uiFont(12.0f, true));
  queueContainer.addAndMakeVisible(mapsLabel);
  queueContainer.addAndMakeVisible(setsList.get());
  addChildComponent(queueContainer);

  ThemeManager::getInstance().addChangeListener(this);
  applyTheme();
  setContext(Context::Songs);
  setSize(240, 400);
}

LibraryPanel::~LibraryPanel() {
  ThemeManager::getInstance().removeChangeListener(this);
}

juce::String LibraryPanel::contextTitle(Context c) {
  switch (c) {
    case Context::Songs:   return "Songs";
    case Context::Strips:  return "Strips";
    case Context::Setlist: return "Setlist";
    case Context::Queue:   return "Queue";
    case Context::Midi:    return "MIDI monitor";
    default:               return {};
  }
}

juce::Component *LibraryPanel::contentFor(Context c) const {
  switch (c) {
    case Context::Songs:   return setupsList.get();
    case Context::Strips:  return stripsList.get();
    case Context::Setlist: return setlistPanel.get();
    case Context::Queue:   return const_cast<juce::Component *>(&queueContainer);
    case Context::Midi:    return midiMonitor;
    default:               return nullptr;
  }
}

bool LibraryPanel::contextHasSearch() const {
  return context == Context::Songs || context == Context::Strips || context == Context::Queue;
}

void LibraryPanel::setContext(Context c) {
  context = c;
  for (int i = 0; i < (int)Context::NumContexts; ++i)
    if (auto *comp = contentFor((Context)i))
      comp->setVisible((Context)i == c);
  titleLabel.setText(contextTitle(c), juce::dontSendNotification);
  searchEditor.setVisible(contextHasSearch());
  newFolderBtn.setVisible(contextHasSearch());
  refreshAll();
  resized();
  repaint();
}

void LibraryPanel::setQueueContent(juce::Component *tiles) {
  queueTiles = tiles;
  if (queueTiles != nullptr)
    queueContainer.addAndMakeVisible(queueTiles);
  resized();
}

void LibraryPanel::setMidiMonitorContent(juce::Component *monitor) {
  midiMonitor = monitor;
  if (midiMonitor != nullptr) {
    addChildComponent(midiMonitor);
    midiMonitor->setVisible(context == Context::Midi);
  }
  resized();
}

void LibraryPanel::changeListenerCallback(juce::ChangeBroadcaster*) {
  applyTheme();
  refreshAll();
  repaint();
}

void LibraryPanel::applyTheme() {
  const auto &t = ThemeManager::getInstance().active();
  searchEditor.setTextToShowWhenEmpty(ThemeManager::isStudio() ? "Search" : "Filter library...", t.textFaint);
  searchEditor.setColour(juce::TextEditor::backgroundColourId, t.dark ? t.sunken : t.raised);
  searchEditor.setColour(juce::TextEditor::outlineColourId, t.dark ? t.border : t.borderStrong);
  searchEditor.setColour(juce::TextEditor::focusedOutlineColourId, t.accent);
  searchEditor.setColour(juce::TextEditor::textColourId, t.text);
  searchEditor.applyColourToAllText(t.text, true);
  titleLabel.setColour(juce::Label::textColourId, t.text);
  countLabel.setColour(juce::Label::textColourId, t.textFaint);
  mapsLabel.setColour(juce::Label::textColourId, t.textDim);
}

void LibraryPanel::refreshAll() {
  if (setsList)
    setsList->refresh();
  if (setupsList)
    setupsList->refresh();
  if (stripsList)
    stripsList->refresh();

  int n = -1;
  juce::String unit;
  switch (context) {
    case Context::Songs:   n = setupsList ? setupsList->getFileCount() : 0; unit = "songs"; break;
    case Context::Strips:  n = stripsList ? stripsList->getFileCount() : 0; unit = "strips"; break;
    case Context::Queue:   n = setsList ? setsList->getFileCount() : 0; unit = "maps"; break;
    case Context::Setlist: n = Fanfare::SetlistManager::getInstance().getSetups().size(); unit = "in set"; break;
    default: break;
  }
  countLabel.setText(n >= 0 ? juce::String(n) + " " + unit : juce::String(), juce::dontSendNotification);
}

void LibraryPanel::visibilityChanged() {
  juce::Component::visibilityChanged();
  refreshAll();
}

juce::File LibraryPanel::getSelectedPresetFile() const {
  if (context == Context::Strips && stripsList != nullptr)
    return stripsList->getSelectedFile();
  return {};
}

bool LibraryPanel::isLearning() const {
  return setupsList && setupsList->learningActive.load(std::memory_order_acquire);
}

void LibraryPanel::assignLearnedTrigger(SetupMidiTrigger t) {
  if (setupsList && setupsList->learningActive.load(std::memory_order_acquire)) {
    setupsList->learningActive.store(false, std::memory_order_release);
    if (setupsList->learningFile.existsAsFile()) {
      auto fileName = setupsList->learningFile.getFileName();
      SetupMidiTriggers::getInstance().setTrigger(fileName, t);
    }
    setupsList->learningFile = juce::File{};
    setupsList->repaint();
  }
}

void LibraryPanel::resized() {
  auto b = getLocalBounds().reduced(8, 6);
  auto header = b.removeFromTop(26);
  countLabel.setBounds(header.removeFromRight(80));
  titleLabel.setBounds(header);
  b.removeFromTop(4);

  if (contextHasSearch()) {
    auto row = b.removeFromTop(26);
    newFolderBtn.setBounds(row.removeFromRight(68));
    row.removeFromRight(6);
    searchEditor.setBounds(row);
    b.removeFromTop(6);
  }

  auto content = b.withTrimmedLeft(-8).withTrimmedRight(-8); // lists run edge to edge
  setupsList->setBounds(content);
  stripsList->setBounds(content);
  setlistPanel->setBounds(b);
  if (midiMonitor != nullptr)
    midiMonitor->setBounds(b);

  queueContainer.setBounds(content);
  auto q = queueContainer.getLocalBounds();
  if (queueTiles != nullptr) {
    const int nTiles = FanfareConstants::kNumSetupButtons;
    const int minTilesH = nTiles * 22 + 32;
    const int maxTilesH = nTiles * 30 + 36;
    const int availForTiles = juce::jmax(120, q.getHeight() - 96);
    const int tilesH = juce::jmin(
        maxTilesH,
        juce::jmax(q.getHeight() * 2 / 3, juce::jmin(minTilesH, availForTiles)));
    queueTiles->setBounds(q.removeFromTop(tilesH).reduced(8, 0));
  }
  q.removeFromTop(6);
  mapsLabel.setBounds(q.removeFromTop(20).reduced(8, 0));
  setsList->setBounds(q);
}

void LibraryPanel::paint(juce::Graphics &g) {
  const auto &t = ThemeManager::getInstance().active();
  auto bounds = getLocalBounds().toFloat();
  g.setColour(t.panel);
  if (t.isStudio()) {
    g.fillRect(bounds);
    g.setColour(t.border);
    g.drawVerticalLine(getWidth() - 1, 0.0f, (float)getHeight());
    return;
  }
  g.fillRoundedRectangle(bounds, 3.0f);
  g.setColour(t.border);
  g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);
}
