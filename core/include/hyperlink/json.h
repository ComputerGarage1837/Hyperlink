// Minimal JSON reader: enough for GitHub's release API. Not a validator.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace hl::json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value& operator[](const std::string& key) const;
    const Value& operator[](size_t i) const;
    std::string asString(const std::string& def = "") const { return type == String ? str : def; }
    double asNumber(double def = 0) const { return type == Number ? num : def; }
    bool asBool(bool def = false) const { return type == Bool ? b : def; }
};

bool parse(const std::string& text, Value& out);

// "v1.2.3" / "1.2.3-beta" -> comparable integer (major*1e6 + minor*1e3 + patch).
long long versionCode(const std::string& v);

}  // namespace hl::json
