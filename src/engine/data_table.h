// Aether Engine — data tables: designer-editable JSON spreadsheets (UE
// DataTable / Unity ScriptableObject-table equivalent). A table is named rows
// of typed cells (number / bool / text) under shared columns; gameplay reads
// values by (row, field) through the GetData script nodes or C++ — so tuning
// lives in data files, not in graphs or code.
//
// File format (assets/data/*.json):
//   { "columns": ["health", "speed", "label"],
//     "rows": { "crate": { "health": 20, "label": "Wooden crate" }, ... } }
// "columns" is display order for the editor; missing cells read as defaults.
#pragma once
#include <string>
#include <vector>

namespace ae {

struct DataCell {
    enum Type { Num, Flag, Text } type = Num;
    double num = 0;
    bool flag = false;
    std::string text;

    float asNumber() const {
        return type == Num ? (float)num
               : type == Flag ? (flag ? 1.0f : 0.0f)
                              : (float)std::atof(text.c_str());
    }
    bool asFlag() const {
        return type == Flag ? flag : type == Num ? num != 0 : !text.empty() && text != "false";
    }
    std::string asText() const;
};

class DataTable {
public:
    struct Row {
        std::string name;
        std::vector<std::pair<std::string, DataCell>> cells; // column -> value
    };

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    int rowCount() const { return (int)rows_.size(); }
    const Row* row(const std::string& name) const;
    const DataCell* cell(const std::string& row, const std::string& column) const;
    float number(const std::string& row, const std::string& column, float def = 0.0f) const;
    bool flag(const std::string& row, const std::string& column, bool def = false) const;
    std::string text(const std::string& row, const std::string& column,
                     const std::string& def = std::string()) const;

    // Mutable access for the editor panel.
    std::vector<std::string>& columns() { return columns_; }
    const std::vector<std::string>& columns() const { return columns_; }
    std::vector<Row>& rows() { return rows_; }
    const std::vector<Row>& rows() const { return rows_; }

private:
    std::vector<std::string> columns_; // display/save order
    std::vector<Row> rows_;            // file order
};

} // namespace ae
