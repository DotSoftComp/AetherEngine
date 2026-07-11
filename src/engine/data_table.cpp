#include "data_table.h"
#include "../core/json.h"
#include "../core/log.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ae {

std::string DataCell::asText() const {
    if (type == Text) return text;
    if (type == Flag) return flag ? "true" : "false";
    char buf[32];
    // Trim trailing zeros so 20.0 prints as "20" (matches editor display).
    std::snprintf(buf, sizeof(buf), "%g", num);
    return buf;
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    return out;
}

const DataTable::Row* DataTable::row(const std::string& name) const {
    for (const Row& r : rows_)
        if (r.name == name) return &r;
    return nullptr;
}

const DataCell* DataTable::cell(const std::string& rowName, const std::string& column) const {
    const Row* r = row(rowName);
    if (!r) return nullptr;
    for (const auto& kv : r->cells)
        if (kv.first == column) return &kv.second;
    return nullptr;
}

float DataTable::number(const std::string& r, const std::string& c, float def) const {
    const DataCell* v = cell(r, c);
    return v ? v->asNumber() : def;
}

bool DataTable::flag(const std::string& r, const std::string& c, bool def) const {
    const DataCell* v = cell(r, c);
    return v ? v->asFlag() : def;
}

std::string DataTable::text(const std::string& r, const std::string& c,
                            const std::string& def) const {
    const DataCell* v = cell(r, c);
    return v ? v->asText() : def;
}

bool DataTable::load(const std::string& path) {
    columns_.clear();
    rows_.clear();

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string textData(size, '\0');
    f.read(&textData[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(textData.c_str(), textData.size(), root)) {
        AE_ERROR("[Data] malformed table: %s", path.c_str());
        return false;
    }

    if (const JsonValue* cols = root.find("columns"))
        for (size_t i = 0; i < cols->size(); ++i)
            if ((*cols)[i].type == JsonValue::String) columns_.push_back((*cols)[i].str);

    auto ensureColumn = [&](const std::string& name) {
        for (const std::string& c : columns_)
            if (c == name) return;
        columns_.push_back(name);
    };

    if (const JsonValue* rows = root.find("rows")) {
        for (const auto& kv : rows->obj) {
            Row r;
            r.name = kv.first;
            for (const auto& field : kv.second.obj) {
                DataCell cell;
                const JsonValue& v = field.second;
                if (v.type == JsonValue::Number) { cell.type = DataCell::Num; cell.num = v.number; }
                else if (v.type == JsonValue::Bool) { cell.type = DataCell::Flag; cell.flag = v.boolean; }
                else { cell.type = DataCell::Text; cell.text = v.str; }
                r.cells.emplace_back(field.first, cell);
                ensureColumn(field.first);
            }
            rows_.push_back(std::move(r));
        }
    }
    return true;
}

bool DataTable::save(const std::string& path) const {
    std::ostringstream o;
    o << "{\n  \"columns\": [";
    for (size_t i = 0; i < columns_.size(); ++i)
        o << (i ? ", " : "") << '"' << jsonEscape(columns_[i]) << '"';
    o << "],\n  \"rows\": {\n";
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        o << "    \"" << jsonEscape(r.name) << "\": { ";
        // Save in column order so files diff cleanly.
        bool first = true;
        for (const std::string& col : columns_) {
            for (const auto& kv : r.cells) {
                if (kv.first != col) continue;
                if (!first) o << ", ";
                first = false;
                o << '"' << jsonEscape(col) << "\": ";
                const DataCell& c = kv.second;
                if (c.type == DataCell::Num) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%g", c.num);
                    o << buf;
                } else if (c.type == DataCell::Flag) {
                    o << (c.flag ? "true" : "false");
                } else {
                    o << '"' << jsonEscape(c.text) << '"';
                }
                break;
            }
        }
        o << " }" << (i + 1 < rows_.size() ? "," : "") << "\n";
    }
    o << "  }\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    std::string textData = o.str();
    f.write(textData.data(), (std::streamsize)textData.size());
    return f.good();
}

} // namespace ae
