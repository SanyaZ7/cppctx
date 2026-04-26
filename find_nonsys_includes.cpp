#include "find_nonsys_includes.h"

#include <algorithm>
#include <cstdlib>          // EXIT_SUCCESS / EXIT_FAILURE
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ */
/* 1. Приведение относительного пути к абсолютному (с «слабой» канонизацией)   */
/* ------------------------------------------------------------------ */
static fs::path resolve_to_absolute(const std::string& p,
                                    const fs::path& base)
{
    fs::path cur{p};

    if (!cur.is_absolute())
        cur = base / cur;

    try {
        return fs::weakly_canonical(cur);
    } catch (const fs::filesystem_error&) {
        /* если что‑то пошло не так – возвращаем как есть */
        return cur;
    }
}

/* ------------------------------------------------------------------ */
/* 2. Информация о «системных» каталогах для каждой ОС                 */
/* ------------------------------------------------------------------ */
struct OSInfo
{
    const char* name;                // название ОС (для отладки)
    std::vector<std::string> roots;  // префиксы, считающиеся системными
};

static const std::map<std::string, OSInfo> os_info = []{
    std::map<std::string, OSInfo> m;

    /* Linux */
    m.emplace("Linux", OSInfo{
        "Linux",
        {"/usr/include",
         "/usr/local/include",
         "/opt/include"}});

    /* macOS (Darwin) */
    m.emplace("Darwin", OSInfo{
        "Darwin",
        {"/Library/Frameworks/Headers",
         "/System/Library/Frameworks",
         "/usr/local/include"}});

    /* Windows – пример с «wildcard» *  */
    m.emplace("Windows", OSInfo{
        "Windows",
        {"C:\\Program Files\\Microsoft Visual Studio\\VC\\Tools\\MSVC\\*"}});

    return m;
}();

/* ------------------------------------------------------------------ */
/* 3. Определяем текущую ОС                                            */
/* ------------------------------------------------------------------ */
static std::string current_os()
{
#if defined(__linux__)
    return "Linux";
#elif defined(__APPLE__) && defined(__MACH__)
    return "Darwin";
#elif defined(_WIN32)
    return "Windows";
#else
    return "Unknown";
#endif
}

/* ------------------------------------------------------------------ */
/* 4. Проверяем, является ли путь «системным»                          */
/* ------------------------------------------------------------------ */
static bool is_system_path(const fs::path& p)
{
    const std::string os = current_os();
    auto it = os_info.find(os);
    if (it == os_info.end())
        return false;                     // неизвестная ОС → «не системный»

    const std::string path_str = p.string();

    for (const auto& root : it->second.roots) {
        /* В Windows допускается * в конце – берём только префикс */
        const std::size_t star = root.find('*');
        const std::string prefix = (star == std::string::npos)
                                       ? root
                                       : root.substr(0, star);
        if (path_str.starts_with(prefix))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* 5. Парсеры конфигурационных файлов                                  */
/* ------------------------------------------------------------------ */

/* Makefile – ищем -I <путь> */
static std::set<std::string> parse_makefile(const fs::path& /*cfg*/,
                                            const std::string& content)
{
    static const std::regex re(R"(-I\s+([^\s\r\n]+))");
    std::set<std::string> result;

    for (auto it = std::sregex_iterator(content.begin(), content.end(), re);
         it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        if (!m[1].str().empty())
            result.insert(m[1].str());          // только строка, без fs::absolute
    }
    return result;
}

/* CMakeLists.txt – ищем include_directories и target_include_directories */
static std::set<std::string> parse_cmake(const fs::path& /*cfg*/,
                                         const std::string& content)
{
    static const std::regex re_inc(R"(include_directories\s*$([^)]*)$)");
    static const std::regex re_tgt(
        R"(target_include_directories\s*\w+\s+(?:PUBLIC|PRIVATE|INTERFACE)?\s+([^)]*))");

    std::set<std::string> result;

    auto parse_dirs = [](const std::smatch& m, std::set<std::string>& res)
    {
        std::istringstream iss(m[1].str());
        std::string dir;
        while (iss >> dir)
            res.insert(dir);
    };

    for (auto it = std::sregex_iterator(content.begin(), content.end(), re_inc);
         it != std::sregex_iterator(); ++it) {
        parse_dirs(*it, result);
    }

    for (auto it = std::sregex_iterator(content.begin(), content.end(), re_tgt);
         it != std::sregex_iterator(); ++it) {
        parse_dirs(*it, result);
    }
    return result;
}

/* .pro – ищем INCLUDEPATH += <путь> */
static std::set<std::string> parse_qmake(const fs::path& /*cfg*/,
                                         const std::string& content)
{
    static const std::regex re(R"(INCLUDEPATH\s*\+=\s+([^\s\r\n]+))");
    std::set<std::string> result;

    for (auto it = std::sregex_iterator(content.begin(), content.end(), re);
         it != std::sregex_iterator(); ++it) {
        const auto& m = *it;
        if (!m[1].str().empty())
            result.insert(m[1].str());
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* 6. Сбор всех каталогов включения (относительных строк)             */
/* ------------------------------------------------------------------ */
static std::vector<fs::path> collect_all_dirs(const fs::path& project_root)
{
    /* Шаг 1 – поиск конфигурационных файлов */
    struct ConfigFile
    {
        fs::path path;
        std::set<std::string> dirs;   // найденные строки (относительные)
    };
    std::vector<ConfigFile> cfg_files;

    for (const auto& entry :
         fs::recursive_directory_iterator(project_root,
                                          fs::directory_options::skip_permission_denied))
    {
        if (!entry.is_regular_file())
            continue;

        const auto& p = entry.path();

        if ((p.filename() == "Makefile" || p.extension() == ".mk") ||
            (p.filename() == "CMakeLists.txt") ||
            (p.extension() == ".pro"))
        {
            cfg_files.push_back({p, {}});
        }
    }

    /* Шаг 2 – чтение файлов и заполнение наборов строк */
    for (auto& cfg : cfg_files) {
        std::ifstream in(cfg.path);
        if (!in)
            continue;

        const std::string content{std::istreambuf_iterator<char>(in),
                                  std::istreambuf_iterator<char>()};

        if (cfg.path.filename() == "Makefile" || cfg.path.extension() == ".mk")
            cfg.dirs = parse_makefile(cfg.path, content);
        else if (cfg.path.filename() == "CMakeLists.txt")
            cfg.dirs = parse_cmake(cfg.path, content);
        else /* *.pro */
            cfg.dirs = parse_qmake(cfg.path, content);
    }

    /* Шаг 3 – привязка относительных строк к абсолютным путям */
    std::set<fs::path> all_dirs;
    for (const auto& cfg : cfg_files) {
        for (const auto& rel_str : cfg.dirs) {
            fs::path abs = resolve_to_absolute(rel_str, cfg.path.parent_path());
            all_dirs.insert(abs);
        }
    }

    return std::vector<fs::path>{all_dirs.begin(), all_dirs.end()};
}

/* ------------------------------------------------------------------ */
/* 7. Публичный API – возвращает вектор строк (абсолютных путей)      */
/* ------------------------------------------------------------------ */
std::optional<std::vector<std::string>>
find_nonsystem_include_dirs(const fs::path& project_root)
{
    if (!fs::exists(project_root) || !fs::is_directory(project_root)) {
        std::cerr << "Error: '" << project_root
                  << "' – не существующая директория\n";
        return std::nullopt;
    }

    const auto dirs = collect_all_dirs(project_root);

    std::vector<std::string> result;
    for (const auto& d : dirs) {
        if (!is_system_path(d))
            result.emplace_back(d.string());
    }

    if (result.empty())
        return std::nullopt;

    return result;
}

