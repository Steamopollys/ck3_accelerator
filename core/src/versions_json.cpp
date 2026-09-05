#include "versions_json.h"

#include <fstream>
#include <sstream>

namespace ck3accel {

namespace {
    struct ParseError {};

    class Parser {
    public:
        explicit Parser(std::string s) : src_(std::move(s)) {}

        std::vector<VersionsEntry> parse() {
            std::vector<VersionsEntry> out;
            skip_ws();
            expect('{');
            skip_ws();
            if (peek() == '}') { advance(); return out; }
            for (;;) {
                std::string key = parse_string();
                skip_ws(); expect(':'); skip_ws();
                out.push_back(parse_entry(key));
                skip_ws();
                if (peek() == ',') { advance(); skip_ws(); continue; }
                break;
            }
            skip_ws();
            expect('}');
            return out;
        }

    private:
        std::string src_;
        std::size_t i_ = 0;

        char peek() {
            if (i_ >= src_.size()) throw ParseError{};
            return src_[i_];
        }
        void advance() { ++i_; }
        void expect(char c) {
            if (i_ >= src_.size() || src_[i_] != c) throw ParseError{};
            ++i_;
        }
        void skip_ws() {
            while (i_ < src_.size() &&
                   (src_[i_] == ' ' || src_[i_] == '\n' ||
                    src_[i_] == '\r' || src_[i_] == '\t')) ++i_;
        }
        std::string parse_string() {
            expect('"');
            std::string s;
            while (i_ < src_.size() && src_[i_] != '"') {
                if (src_[i_] == '\\' && i_ + 1 < src_.size()) {
                    s.push_back(src_[i_ + 1]);
                    i_ += 2;
                } else {
                    s.push_back(src_[i_++]);
                }
            }
            expect('"');
            return s;
        }
        bool parse_bool() {
            if (src_.compare(i_, 4, "true") == 0)  { i_ += 4; return true; }
            if (src_.compare(i_, 5, "false") == 0) { i_ += 5; return false; }
            throw ParseError{};
        }
        std::int64_t parse_int() {
            std::size_t start = i_;
            if (src_[i_] == '-') ++i_;
            while (i_ < src_.size() && src_[i_] >= '0' && src_[i_] <= '9') ++i_;
            if (start == i_) throw ParseError{};
            return std::stoll(src_.substr(start, i_ - start));
        }
        std::vector<std::string> parse_string_array() {
            std::vector<std::string> out;
            expect('[');
            skip_ws();
            if (peek() == ']') { advance(); return out; }
            for (;;) {
                out.push_back(parse_string());
                skip_ws();
                if (peek() == ',') { advance(); skip_ws(); continue; }
                break;
            }
            expect(']');
            return out;
        }
        VersionsEntry parse_entry(const std::string& version) {
            VersionsEntry e;
            e.version = version;
            expect('{'); skip_ws();
            bool first = true;
            while (peek() != '}') {
                if (!first) { expect(','); skip_ws(); }
                first = false;
                std::string field = parse_string();
                skip_ws(); expect(':'); skip_ws();
                if (field == "pe_timestamp") {
                    if (peek() == '"') {
                        // placeholder string: leave 0 to signal skip
                        (void)parse_string();
                        e.pe_timestamp = 0;
                    } else {
                        e.pe_timestamp = static_cast<std::uint32_t>(parse_int());
                    }
                } else if (field == "text_sha256") {
                    e.text_sha256 = parse_string();
                } else if (field == "tested") {
                    e.tested = parse_bool();
                } else if (field == "auto_disable") {
                    e.auto_disable = parse_string_array();
                } else {
                    // unknown field: skip its value (string/bool/int/array only)
                    if (peek() == '"') (void)parse_string();
                    else if (peek() == '[') (void)parse_string_array();
                    else if (peek() == 't' || peek() == 'f') (void)parse_bool();
                    else (void)parse_int();
                }
                skip_ws();
            }
            expect('}');
            return e;
        }
    };
}

std::optional<std::vector<VersionsEntry>>
load_versions_json(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    try {
        Parser p(ss.str());
        auto raw = p.parse();
        // drop placeholders: pe_timestamp 0 and text_sha256 starts with "PENDING_".
        std::vector<VersionsEntry> usable;
        for (auto& e : raw) {
            bool placeholder =
                (e.pe_timestamp == 0 && e.text_sha256.rfind("PENDING_", 0) == 0);
            if (!placeholder) usable.push_back(std::move(e));
        }
        return usable;
    } catch (...) {
        return std::nullopt;
    }
}

const VersionsEntry* find_match(
    const std::vector<VersionsEntry>& entries,
    std::uint32_t pe_timestamp,
    const std::string& text_sha256)
{
    for (const auto& e : entries) {
        if (e.pe_timestamp == pe_timestamp && e.text_sha256 == text_sha256) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace ck3accel
