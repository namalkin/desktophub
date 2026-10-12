// Минимальный JSON: ровно столько, сколько нужно для data.json.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace mj {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;  // UTF-8
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    static Value Object() { Value v; v.type = Type::Object; return v; }
    static Value Array() { Value v; v.type = Type::Array; return v; }
    static Value Num(double n) { Value v; v.type = Type::Number; v.number = n; return v; }
    static Value Str(std::string s) { Value v; v.type = Type::String; v.str = std::move(s); return v; }
    static Value Bool(bool b) { Value v; v.type = Type::Bool; v.boolean = b; return v; }

    void Set(const std::string& key, Value v) { obj.emplace_back(key, std::move(v)); }
    const Value* Find(const std::string& key) const;

    double NumOr(double fallback) const { return type == Type::Number ? number : fallback; }
    bool BoolOr(bool fallback) const { return type == Type::Bool ? boolean : fallback; }
};

bool Parse(const std::string& text, Value& out);
std::string Serialize(const Value& v);

}  // namespace mj
