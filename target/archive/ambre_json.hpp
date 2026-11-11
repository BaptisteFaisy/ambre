/*
 * ambre_json.hpp — minimal JSON object builder used by the MCAP mapping.
 *
 * The archive writes JSON payloads (docs/FORMAT.md §12: encoding `json` with
 * JSON schemas). No external JSON library is available on the target, so this
 * header provides exactly what the exporter needs: a flat object of scalar
 * values, emitted with sorted keys so two runs give byte-identical output.
 *
 * Header-only on purpose: it is a formatting helper, not a module with state.
 */

#ifndef AMBRE_JSON_HPP
#define AMBRE_JSON_HPP

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace ambre {
namespace json {

inline std::string escape(const std::string& in)
{
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (static_cast<unsigned char>(c) < 0x20u) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
            out.append(buf);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/* Numbers: the shortest decimal that round-trips (same algorithm as Python's
 * repr, which json.dumps uses), with a trailing ".0" for integral values. This
 * is what keeps the C++ and the host Python exports byte-identical. */
inline std::string number(double value)
{
    char buf[32];
    for (int precision = 1; precision <= 17; precision++) {
        std::snprintf(buf, sizeof(buf), "%.*g", precision, value);
        if (std::strtod(buf, nullptr) == value) {
            break;
        }
    }
    std::string text(buf);
    if (text.find_first_of(".eE") == std::string::npos) {
        text.append(".0");
    }
    return text;
}

class Object {
 public:
    void integer(const std::string& key, long long value)
    {
        fields_[key] = std::to_string(value);
    }
    void number(const std::string& key, double value)
    {
        fields_[key] = json::number(value);
    }
    void string(const std::string& key, const std::string& value)
    {
        fields_[key] = "\"" + escape(value) + "\"";
    }
    void raw(const std::string& key, const std::string& value)
    {
        fields_[key] = value;
    }
    std::string text() const
    {
        std::string out = "{";
        bool first = true;
        for (const auto& entry : fields_) {
            if (!first) {
                out.append(", ");
            }
            first = false;
            out.append("\"" + escape(entry.first) + "\": " + entry.second);
        }
        return out + "}";
    }

 private:
    std::map<std::string, std::string> fields_;
};

}  // namespace json
}  // namespace ambre

#endif /* AMBRE_JSON_HPP */
