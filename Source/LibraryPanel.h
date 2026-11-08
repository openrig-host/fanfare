#pragma once

#include <JuceHeader.h>
#include "RigLibrary.h"
#include <set>

class LibraryList : public juce::ListBox,
                    public juce::ListBoxModel {
public:
  enum class Kind { Sets, Setups, Strips };

  struct Item {
    juce::File file;
    juce::String displayName;
    juce::String category;
    bool isFolderHeader = false;
    bool isCollapsed = false;
    bool isFavorite = false;
    juce::Colour badgeColour;
    juce::File folderDir;
  };

  explicit LibraryList(Kind k);
  void setDirectory(const juce::File &dir, const juce::String &wildcard);
  void setFilterQuery(const juce::String &query);
  void refresh();

  int getNumRows() override;
  void paintListBoxItem(int row, juce::Graphics &g, int width, int height,
                        bool isSelected) override;

  void selectedRowsChanged(int) override;
  void listBoxItemClicked(int row, const juce::MouseEvent &) override;
  void listBoxItemDoubleClicked(int row, const juce::MouseEvent &) override;

  juce::var getDragSourceDescription(const juce::SparseSet<int>& selectedRows) override;

  juce::File getSelectedFile() const;
  juce::File getDirectory() const { return directory; }
  int getFileCount() const { return (int)allFiles.size(); }

  void toggleFolderCollapsed(const juce::String &category);
  void createNewFolder();
  void moveItemToFolder(const juce::File &file, const juce::String &targetSubfolder);

  std::function<void(const juce::File &)> onDoubleClicked;

  Kind kind;
  juce::File learningFile;
  std::atomic<bool> learningActive{false};

private:
  juce::File directory;
  juce::String wc;
  juce::String filterQuery;
  std::vector<juce::File> allFiles;
  std::vector<Item> items;
  std::set<juce::String> favorites;
  std::set<juce::String> collapsedFolders;
  int selectedRow = -1;

  void loadFavorites();
  void saveFavorites();
  void toggleFavorite(const juce::File &f);
  juce::Colour detectBadgeColour(const juce::String &name, Kind k);
};

#include "SetlistPanel.h"

// LibraryPanel - the context sidebar beside the ToolRail. One context is
// shown at a time (Songs, Strips, Setlist, Queue, MIDI monitor); the rail
// switches it. Replaces the old tab strip ("vertical ribbon" layout).
class LibraryPanel : public juce::Component,
                     public juce::DragAndDropContainer,
                     private juce::ChangeListener {
public:
  enum class Context { Songs = 0, Strips, Setlist, Queue, Midi, NumContexts };

  LibraryPanel();
  ~LibraryPanel() override;

  void refreshAll();
  void visibilityChanged() override;

  juce::File getSelectedPresetFile() const;
  void resized() override;
  void paint(juce::Graphics &g) override;

  bool isLearning() const;
  void assignLearnedTrigger(struct SetupMidiTrigger t);

  void setContext(Context c);
  Context getContext() const { return context; }
  static juce::String contextTitle(Context c);

  // Hosted content owned elsewhere (MainComponent): the queue tiles panel is
  // shown above the saved queue maps; the MIDI monitor fills its context.
  void setQueueContent(juce::Component *queueTiles);
  void setMidiMonitorContent(juce::Component *monitor);

  std::function<void(const juce::File &)> onSetupDoubleClicked;
  std::function<void(const juce::File &)> onSetDoubleClicked;

  Fanfare::SetlistPanel* getSetlistPanel() { return setlistPanel.get(); }

private:
  void changeListenerCallback(juce::ChangeBroadcaster*) override;
  void applyTheme();
  juce::Component *contentFor(Context c) const;
  bool contextHasSearch() const;

  Context context = Context::Songs;
  juce::Label titleLabel, countLabel, mapsLabel;
  juce::TextEditor searchEditor;
  juce::TextButton newFolderBtn;
  std::unique_ptr<LibraryList> setsList, setupsList, stripsList;
  std::unique_ptr<Fanfare::SetlistPanel> setlistPanel;
  juce::Component queueContainer;
  juce::Component *queueTiles = nullptr;
  juce::Component *midiMonitor = nullptr;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LibraryPanel)
};
