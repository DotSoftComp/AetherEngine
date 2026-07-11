#include "data_table_panel.h"
#include "../engine/assets.h"
#include "../core/log.h"
#include "imgui.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace ae {

// Parse a committed cell string back into a typed cell.
static DataCell parseCell(const char* s) {
    DataCell c;
    if (!std::strcmp(s, "true") || !std::strcmp(s, "false")) {
        c.type = DataCell::Flag;
        c.flag = s[0] == 't';
        return c;
    }
    char* end = nullptr;
    double n = std::strtod(s, &end);
    if (end && end != s && *end == '\0') {
        c.type = DataCell::Num;
        c.num = n;
        return c;
    }
    c.type = DataCell::Text;
    c.text = s;
    return c;
}

bool DataTablePanel::createStarterTable(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const char* starter =
        "{\n  \"columns\": [\"value\", \"label\"],\n"
        "  \"rows\": {\n    \"example\": { \"value\": 1, \"label\": \"Edit me\" }\n  }\n}\n";
    f << starter;
    return f.good();
}

void DataTablePanel::open(const std::string& absPath, bool focus) {
    if (!table_.load(absPath)) {
        AE_ERROR("[Data] can't open table: %s", absPath.c_str());
        return;
    }
    path_ = absPath;
    loaded_ = true;
    dirty_ = false;
    visible = true;
    focusRequested_ = focus;
}

bool DataTablePanel::save(AssetLibrary* assets) {
    if (!loaded_ || !table_.save(path_)) return false;
    dirty_ = false;
    if (assets) assets->reloadDataTable(path_); // live GetData nodes see it now
    AE_LOG("[Data] saved %s", path_.c_str());
    return true;
}

void DataTablePanel::draw(AssetLibrary* assets) {
    if (focusRequested_) {
        ImGui::SetNextWindowFocus();
        focusRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 420), ImGuiCond_FirstUseEver);
    std::string title = "Data Table";
    if (loaded_) {
        size_t slash = path_.find_last_of("/\\");
        title += " - " + (slash == std::string::npos ? path_ : path_.substr(slash + 1));
        if (dirty_) title += " *";
    }
    title += "###DataTable"; // stable window id across file switches
    if (!ImGui::Begin(title.c_str(), &visible)) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("Double-click a table in the Content Browser (assets/data/*.json).");
        ImGui::End();
        return;
    }

    // Toolbar
    bool ctrlS = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                 ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
    if (ImGui::Button("Save") || ctrlS) save(assets);
    ImGui::SameLine();
    if (ImGui::Button("+ Row")) {
        DataTable::Row r;
        r.name = "row" + std::to_string(table_.rowCount());
        table_.rows().push_back(r);
        dirty_ = true;
    }
    ImGui::SameLine();
    static char newCol[64] = "";
    ImGui::SetNextItemWidth(120);
    bool commitCol = ImGui::InputText("##newcol", newCol, sizeof(newCol),
                                      ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("+ Column") || commitCol) && newCol[0]) {
        bool exists = false;
        for (const std::string& c : table_.columns()) exists |= c == newCol;
        if (!exists) {
            table_.columns().push_back(newCol);
            dirty_ = true;
        }
        newCol[0] = '\0';
    }
    ImGui::SameLine();
    ImGui::TextDisabled("cells: number, true/false, or text  -  read with GetData nodes");

    // Grid
    auto& cols = table_.columns();
    int nCols = (int)cols.size();
    if (ImGui::BeginTable("##grid", nCols + 2,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Row", ImGuiTableColumnFlags_WidthFixed, 120);
        for (const std::string& c : cols) ImGui::TableSetupColumn(c.c_str());
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 24);

        // Header row (right-click a column header to delete the column).
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        ImGui::TableSetColumnIndex(0);
        ImGui::TableHeader("Row");
        int killCol = -1;
        for (int ci = 0; ci < nCols; ++ci) {
            ImGui::TableSetColumnIndex(ci + 1);
            ImGui::PushID(ci);
            ImGui::TableHeader(cols[ci].c_str());
            if (ImGui::BeginPopupContextItem("##colctx")) {
                if (ImGui::MenuItem(("Delete column '" + cols[ci] + "'").c_str())) killCol = ci;
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::TableSetColumnIndex(nCols + 1);
        ImGui::TableHeader("");

        int killRow = -1;
        for (int ri = 0; ri < table_.rowCount(); ++ri) {
            DataTable::Row& row = table_.rows()[ri];
            ImGui::TableNextRow();
            ImGui::PushID(ri);

            ImGui::TableSetColumnIndex(0);
            char nameBuf[64];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", row.name.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
                row.name = nameBuf;
                dirty_ = true;
            }

            for (int ci = 0; ci < nCols; ++ci) {
                ImGui::TableSetColumnIndex(ci + 1);
                ImGui::PushID(ci);
                DataCell* cell = nullptr;
                for (auto& kv : row.cells)
                    if (kv.first == cols[ci]) cell = &kv.second;
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", cell ? cell->asText().c_str() : "");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##cell", buf, sizeof(buf))) {
                    if (!cell) {
                        row.cells.emplace_back(cols[ci], DataCell{});
                        cell = &row.cells.back().second;
                    }
                    *cell = parseCell(buf);
                    dirty_ = true;
                }
                ImGui::PopID();
            }

            ImGui::TableSetColumnIndex(nCols + 1);
            if (ImGui::SmallButton("x")) killRow = ri;
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (killRow >= 0) {
            table_.rows().erase(table_.rows().begin() + killRow);
            dirty_ = true;
        }
        if (killCol >= 0) {
            std::string name = cols[killCol];
            cols.erase(cols.begin() + killCol);
            for (auto& row : table_.rows())
                for (size_t i = 0; i < row.cells.size(); ++i)
                    if (row.cells[i].first == name) {
                        row.cells.erase(row.cells.begin() + i);
                        break;
                    }
            dirty_ = true;
        }
    }
    ImGui::End();
}

} // namespace ae
