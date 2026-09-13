#pragma once
// 极简 JSON：只支持本项目协议需要的类型（对象 / 数组 / 字符串 / 数字 / 布尔 / null）。
// 解析把未知字段整棵保留，序列化按插入顺序输出——协议两端字段名固定，够用即可。

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qj {

class Value {
public:
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    // 保持插入序的对象成员（协议字段数量少，线性查找足够）。
    std::vector<std::pair<std::string, Value>> obj;

    static Value makeNull() { return Value(); }
    static Value makeBool(bool v) { Value x; x.type = Bool; x.b = v; return x; }
    static Value makeNumber(double v) { Value x; x.type = Number; x.num = v; return x; }
    static Value makeString(std::string v) { Value x; x.type = String; x.str = std::move(v); return x; }
    static Value makeArray() { Value x; x.type = Array; return x; }
    static Value makeObject() { Value x; x.type = Object; return x; }

    void set(const std::string &key, Value v) {
        for (auto &member : obj) {
            if (member.first == key) {
                member.second = std::move(v);
                return;
            }
        }
        obj.emplace_back(key, std::move(v));
    }

    void push(Value v) { arr.push_back(std::move(v)); }

    const Value *get(const std::string &key) const {
        if (type != Object) return nullptr;
        for (const auto &member : obj) {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }

    const Value *getPath(std::initializer_list<const char *> keys) const {
        const Value *cur = this;
        for (const char *key : keys) {
            cur = cur->get(key);
            if (!cur) return nullptr;
        }
        return cur;
    }

    bool isString() const { return type == String; }
    bool isNumber() const { return type == Number; }
    bool isBool() const { return type == Bool; }
    bool isNull() const { return type == Null; }

    std::string dump() const;
    static bool parse(const std::string &text, Value &out, std::string &error);
};

}  // namespace qj
