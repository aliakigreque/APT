// Minimal json parser for the jsonl files (levels and recordings).

#pragma once
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ap::jsonl {

    struct Value {
        enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
        bool b = false;
        double num = 0;
        std::string str;
        std::vector<Value> arr;
        std::map<std::string, Value> obj;

        // field of an object, nullptr if missing
        Value const* get(std::string const& key) const {
            auto it = obj.find(key);
            return it == obj.end() ? nullptr : &it->second;
        }

        // field as a number, fallback if missing or not a number
        double numOr(std::string const& key, double fallback = 0) const {
            auto* v = get(key);
            return v && v->kind == Number ? v->num : fallback;
        }

        // field as an int, truncated
        int intOr(std::string const& key, int fallback = 0) const { return static_cast<int>(numOr(key, fallback)); }

        // field as a bool, numbers count (non zero is true)
        bool boolOr(std::string const& key, bool fallback = false) const {
            auto* v = get(key);
            if (!v) return fallback;
            if (v->kind == Bool) return v->b;
            if (v->kind == Number) return v->num != 0;
            return fallback;
        }

        // field as a string, fallback if missing or not a string
        std::string strOr(std::string const& key, std::string const& fallback = {}) const {
            auto* v = get(key);
            return v && v->kind == String ? v->str : fallback;
        }
    };

    class Parser {
    public:
        // parser over one line of text
        explicit Parser(std::string_view text) : m_s(text) {}

        // reads one value, spaces around it are allowed
        bool parse(Value& out) {
            skip();
            return value(out) && (skip(), true);
        }

    private:
        std::string_view m_s;
        size_t m_i = 0;

        // skips spaces, tabs and line breaks
        void skip() {
            while (m_i < m_s.size() && (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\n' || m_s[m_i] == '\r'))
                ++m_i;
        }

        // reads any json value
        bool value(Value& out) {
            skip();
            if (m_i >= m_s.size()) return false;
            char c = m_s[m_i];
            if (c == '{') return object(out);
            if (c == '[') return array(out);
            if (c == '"') {
                out.kind = Value::String;
                return string(out.str);
            }
            if (m_s.substr(m_i, 4) == "true") {
                out.kind = Value::Bool;
                out.b = true;
                m_i += 4;
                return true;
            }
            if (m_s.substr(m_i, 5) == "false") {
                out.kind = Value::Bool;
                out.b = false;
                m_i += 5;
                return true;
            }
            if (m_s.substr(m_i, 4) == "null") {
                out.kind = Value::Null;
                m_i += 4;
                return true;
            }
            size_t start = m_i;
            while (m_i < m_s.size() && (std::isdigit(static_cast<unsigned char>(m_s[m_i])) || m_s[m_i] == '-' ||
                                        m_s[m_i] == '+' || m_s[m_i] == '.' || m_s[m_i] == 'e' || m_s[m_i] == 'E' ||
                                        m_s[m_i] == 'i' || m_s[m_i] == 'n' || m_s[m_i] == 'f' || m_s[m_i] == 'a'))
                ++m_i;
            if (m_i == start) return false;
            std::string t(m_s.substr(start, m_i - start));
            out.kind = Value::Number;
            out.num = std::strtod(t.c_str(), nullptr);
            return true;
        }

        // reads a json string, only \n and \t are decoded
        bool string(std::string& out) {
            if (m_s[m_i] != '"') return false;
            ++m_i;
            out.clear();
            while (m_i < m_s.size() && m_s[m_i] != '"') {
                if (m_s[m_i] == '\\' && m_i + 1 < m_s.size()) {
                    ++m_i;
                    char e = m_s[m_i];
                    out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                } else
                    out += m_s[m_i];
                ++m_i;
            }
            if (m_i >= m_s.size()) return false;
            ++m_i;
            return true;
        }

        // reads a json array
        bool array(Value& out) {
            out.kind = Value::Array;
            ++m_i;
            skip();
            if (m_i < m_s.size() && m_s[m_i] == ']') {
                ++m_i;
                return true;
            }
            while (true) {
                Value e;
                if (!value(e)) return false;
                out.arr.push_back(std::move(e));
                skip();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_s[m_i] == ']') {
                    ++m_i;
                    return true;
                }
                return false;
            }
        }

        // reads a json object
        bool object(Value& out) {
            out.kind = Value::Object;
            ++m_i;
            skip();
            if (m_i < m_s.size() && m_s[m_i] == '}') {
                ++m_i;
                return true;
            }
            while (true) {
                skip();
                std::string k;
                if (!string(k)) return false;
                skip();
                if (m_i >= m_s.size() || m_s[m_i] != ':') return false;
                ++m_i;
                Value e;
                if (!value(e)) return false;
                out.obj[k] = std::move(e);
                skip();
                if (m_i >= m_s.size()) return false;
                if (m_s[m_i] == ',') {
                    ++m_i;
                    continue;
                }
                if (m_s[m_i] == '}') {
                    ++m_i;
                    return true;
                }
                return false;
            }
        }
    };

    // parses one line of a .jsonl file
    inline bool parseLine(std::string_view line, Value& out) {
        Parser p(line);
        return p.parse(out);
    }

}
