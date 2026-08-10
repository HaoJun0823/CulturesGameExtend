#include "pch.h"
#include "IniConfig.h"
#include "fs_compat.h"
#include <fstream>
#include <cctype>
#include <algorithm>

namespace {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 去除行内注释（';' 或 '#' 之后全部丢弃，除非在引号内——此处简化处理）
std::string StripComment(const std::string& s) {
    size_t pos = s.find_first_of(";#");
    if (pos == std::string::npos) return s;
    return s.substr(0, pos);
}

} // namespace

std::string IniConfig::Normalize(const std::string& s) {
    std::string r = Trim(s);
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

bool IniConfig::Load(const std::string& path) {
    m_path = path;
    m_data.clear();
    m_loaded = false;

    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string curSection; // 空串表示全局段
    std::string line;
    while (std::getline(in, line)) {
        ge::StripUtf8Bom(line); // 容错：文件带 BOM 时剥除，不带时无影响
        line = Trim(StripComment(line));
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            curSection = Trim(line.substr(1, line.size() - 2));
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;

        m_data[Normalize(curSection)][Normalize(key)] = val;
    }

    m_loaded = true;
    return true;
}

void IniConfig::Merge(const IniConfig& other) {
    for (const auto& secPair : other.m_data) {
        auto& targetSec = m_data[secPair.first]; // 自动创建该段
        for (const auto& kv : secPair.second)
            targetSec[kv.first] = kv.second;     // 覆盖同名 key
    }
    if (other.m_loaded) m_loaded = true;
}

bool IniConfig::HasKey(const std::string& section, const std::string& key) const {
    auto sit = m_data.find(Normalize(section));
    if (sit == m_data.end()) return false;
    return sit->second.find(Normalize(key)) != sit->second.end();
}

std::string IniConfig::GetString(const std::string& section,
                                 const std::string& key,
                                 const std::string& def) const {
    auto sit = m_data.find(Normalize(section));
    if (sit == m_data.end()) return def;
    auto kit = sit->second.find(Normalize(key));
    if (kit == sit->second.end()) return def;
    return kit->second;
}

int IniConfig::GetInt(const std::string& section,
                      const std::string& key, int def) const {
    const std::string& s = GetString(section, key, "");
    if (s.empty()) return def;
    try {
        // 支持 0x 前缀的十六进制
        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            return static_cast<int>(std::stoul(s, nullptr, 16));
        }
        return std::stoi(s);
    } catch (...) {
        return def;
    }
}

bool IniConfig::GetBool(const std::string& section,
                        const std::string& key, bool def) const {
    const std::string& s = Normalize(GetString(section, key, ""));
    if (s.empty()) return def;
    // 支持 1/true/yes/on 为 true；0/false/no/off 为 false
    if (s == "1" || s == "true" || s == "yes" || s == "on")  return true;
    if (s == "0" || s == "false" || s == "no"  || s == "off") return false;
    return def;
}
