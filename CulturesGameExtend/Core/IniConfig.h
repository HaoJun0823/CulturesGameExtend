#pragma once
#include <string>
#include <unordered_map>

// 极简 INI 解析器（零依赖）。
// 约定：
//   [Section]            -> 段开始
//   Key = Value          -> 键值对（忽略首尾空白，';' 或 '#' 开头为注释）
// 段名与键名均不区分大小写进行比较（存储时保留原样）。

// Minimal INI parser (zero-dependency).
// Conventions:
//   [Section]            -> section start
//   Key = Value          -> key/value pair (leading/trailing whitespace ignored;
//                            lines starting with ';' or '#' are comments)
// Section names and key names are compared case-insensitively (original casing
// is preserved when stored).
class IniConfig {
public:
    // 从文件加载；文件不存在返回 false（但对象仍可用，全部取默认值）
    // Load from a file; returns false if the file does not exist (but the object
    // remains usable, yielding all default values).
    bool Load(const std::string& path);

    // 把另一个 IniConfig 的条目并入当前对象：对相同 (section,key)，other 的值覆盖当前值。
    // 用于把 CulturesGameExtend_Game.ini 覆盖到 CulturesGameExtend_Global.ini 之上，
    // 实现"Global 提供默认/共享配置，Game 提供按版本覆盖"。

    // Merge another IniConfig's entries into this object: for the same
    // (section,key), the other's value overrides the current one.
    // Used to layer CulturesGameExtend_Game.ini on top of
    // CulturesGameExtend_Global.ini, achieving "Global supplies defaults/shared
    // config, Game supplies per-version overrides".
    void Merge(const IniConfig& other);


    bool        IsLoaded() const { return m_loaded; }
    std::string FilePath() const { return m_path; }

    // 读取接口，缺省返回默认值

    // Read interfaces; return the default value when missing.
    std::string GetString(const std::string& section, const std::string& key,
                          const std::string& def = "") const;
    int         GetInt(const std::string& section, const std::string& key,
                       int def = 0) const;
    bool        GetBool(const std::string& section, const std::string& key,
                        bool def = false) const;

    // 返回 true 表示 (section,key) 存在

    // Returns true if (section,key) exists.
    bool HasKey(const std::string& section, const std::string& key) const;


private:
    static std::string Normalize(const std::string& s);
// 去空白/转小写
// strip whitespace / lowercase
    bool m_loaded = false;


    std::string m_path;
    std::unordered_map<std::string,
    // map<section, map<key, value>>
    // map<section, map<key, value>>
        std::unordered_map<std::string, std::string>> m_data;
};