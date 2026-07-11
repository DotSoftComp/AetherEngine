// Aether Engine — spreadsheet editor for DataTable assets (assets/data/*.json):
// named rows x typed columns, edited in place. Cells auto-type on commit
// (numeric text -> number, "true"/"false" -> bool, anything else -> text).
// Ctrl+S / Save writes the file and hot-reloads the cached table, so GetData
// script nodes see new values immediately (including during Play).
#pragma once
#include "../engine/data_table.h"
#include <string>

namespace ae {

class AssetLibrary;

class DataTablePanel {
public:
    bool visible = false;

    void open(const std::string& absPath, bool focus = true);
    void draw(AssetLibrary* assets);
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    static bool createStarterTable(const std::string& path);

private:
    bool save(AssetLibrary* assets);

    DataTable table_;
    std::string path_;
    bool loaded_ = false;
    bool dirty_ = false;
    bool focusRequested_ = false;
};

} // namespace ae
