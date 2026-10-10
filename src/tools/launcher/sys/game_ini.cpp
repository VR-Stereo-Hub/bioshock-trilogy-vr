// Adapted from the Dishonored VR launcher; see NOTICE.md for provenance and license.
// tools/installer/sys/game_ini.cpp - see game_ini.h.
#include "sys/game_ini.h"
#include "sys/fs.h"
#include <string.h>

namespace bvr::launcher {

namespace {
std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}
bool ieq(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }
// "[Name]" with surrounding whitespace allowed; returns the name.
bool header_name(const std::string& bodyLine, std::string* name)
{
    const std::string t = trim(bodyLine);
    if (t.size() < 3 || t.front() != '[' || t.back() != ']') return false;
    *name = t.substr(1, t.size() - 2);
    return !name->empty();
}
// "Key = value" -> key and value (both trimmed); false when the line is not key=value.
bool split_kv(const std::string& bodyLine, std::string* key, std::string* value)
{
    const size_t eq = bodyLine.find('=');
    if (eq == std::string::npos) return false;
    *key = trim(bodyLine.substr(0, eq));
    *value = trim(bodyLine.substr(eq + 1));
    return !key->empty();
}
}

std::string GameIni::body(const std::string& line)
{
    size_t n = line.size();
    if (n && line[n - 1] == '\n') --n;
    if (n && line[n - 1] == '\r') --n;
    return line.substr(0, n);
}
std::string GameIni::ending(const std::string& line)
{
    const size_t n = line.size();
    if (n >= 2 && line[n - 2] == '\r' && line[n - 1] == '\n') return "\r\n";
    if (n >= 1 && line[n - 1] == '\n') return "\n";
    return "";
}

bool GameIni::load(const std::wstring& path, GameIni* out, std::wstring* err)
{
    std::vector<uint8_t> bytes;
    DWORD e = 0;
    if (!fs::read_file(path, &bytes, &e)) { if (err) *err = fs::win_error_text(e); return false; }
    out->path_ = path;
    out->parse(bytes);
    return true;
}

void GameIni::parse(const std::vector<uint8_t>& raw)
{
    bom_.clear(); lines_.clear(); utf16_ = false; dirty_ = false;
    std::string text;
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        utf16_ = true;
        bom_ = { 0xFF, 0xFE };
        const std::wstring w((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
        text = fs::narrow(w);
    } else if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        bom_ = { 0xEF, 0xBB, 0xBF };
        text.assign((const char*)raw.data() + 3, raw.size() - 3);
    } else {
        text.assign((const char*)raw.data(), raw.size());
    }
    size_t crlf = 0, lf = 0, start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines_.push_back(text.substr(start, i + 1 - start));
            if (i > 0 && text[i - 1] == '\r') ++crlf; else ++lf;
            start = i + 1;
        }
    }
    if (start < text.size()) lines_.push_back(text.substr(start));
    eol_ = (lf > crlf) ? "\n" : "\r\n";
}

bool GameIni::find_section(const std::string& name, Section* out) const
{
    size_t header = (size_t)-1;
    std::string n;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (!header_name(body(lines_[i]), &n)) continue;
        if (header != (size_t)-1) { out->header = header; out->end = i; return true; }
        if (ieq(n, name)) header = i;
    }
    if (header == (size_t)-1) return false;
    out->header = header; out->end = lines_.size();
    return true;
}

GameIni::Result GameIni::set(const std::string& section, const std::string& key, const std::string& value, std::string* note)
{
    Section s;
    if (!find_section(section, &s)) {
        if (note) *note = fs::format("[%s] not found - the file is not the shape this expects, nothing was written", section.c_str());
        return Result::NoSection;
    }
    std::string k, v;
    for (size_t i = s.header + 1; i < s.end; ++i) {
        if (!split_kv(body(lines_[i]), &k, &v) || !ieq(k, key)) continue;
        if (v == value) {   // case-sensitive: FALSE is not False to the engine's reader
            if (note) *note = fs::format("[%s] %s=%s already set", section.c_str(), key.c_str(), value.c_str());
            return Result::Unchanged;
        }
        if (note) *note = fs::format("[%s] %s: %s -> %s", section.c_str(), key.c_str(), v.c_str(), value.c_str());
        lines_[i] = key + "=" + value + ending(lines_[i]);
        dirty_ = true;
        return Result::Changed;
    }
    // A key the file does not carry goes at the end of its section, after
    // the last non-blank line, so the engine reads it like any other.
    size_t at = s.end;
    while (at > s.header + 1 && trim(body(lines_[at - 1])).empty()) --at;
    if (at > 0 && ending(lines_[at - 1]).empty()) lines_[at - 1] += eol_;   // the last line had no newline
    lines_.insert(lines_.begin() + at, key + "=" + value + eol_);
    if (note) *note = fs::format("[%s] %s=%s was MISSING - appended at the end of the section", section.c_str(), key.c_str(), value.c_str());
    dirty_ = true;
    return Result::Changed;
}

bool GameIni::get_unique(const std::string& section, const std::string& key, std::string* value) const {
    bool inside = false;
    unsigned count = 0, sections = 0;
    for (const auto& line : lines_) {
        std::string name, k, v;
        if (header_name(body(line), &name)) {
            inside = ieq(name, section);
            if (inside) ++sections;
        } else if (inside && split_kv(body(line), &k, &v) && ieq(k, key)) {
            *value = v; ++count;
        }
    }
    return sections == 1 && count == 1;
}
void GameIni::ensure_section(const std::string& section) {
    Section found;
    if (find_section(section, &found)) return;
    if (!lines_.empty() && ending(lines_.back()).empty()) lines_.back() += eol_;
    lines_.push_back("[" + section + "]" + eol_);
    dirty_ = true;
}

std::vector<uint8_t> GameIni::serialise() const
{
    std::string text;
    for (const auto& l : lines_) text += l;
    std::vector<uint8_t> out(bom_.begin(), bom_.end());
    if (utf16_) {
        const std::wstring w = fs::widen(text);
        const uint8_t* p = (const uint8_t*)w.data();
        out.insert(out.end(), p, p + w.size() * 2);
    } else {
        out.insert(out.end(), text.begin(), text.end());
    }
    return out;
}

bool GameIni::save(std::wstring* err)
{
    if (!dirty_) return true;
    const std::vector<uint8_t> bytes = serialise();
    DWORD e = 0;
    if (!fs::write_file_atomic(path_, bytes.data(), bytes.size(), &e)) { if (err) *err = fs::win_error_text(e); return false; }
    dirty_ = false;
    return true;
}

} // namespace bvr::launcher
