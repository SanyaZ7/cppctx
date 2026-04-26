/*********************************************************************
 *  context_builder.cpp
 *
 *   Сборка «контекста» для *.cpp* и *.h / *.hpp* файлов:
 *      • читаем один (или несколько) входных файлов;
 *      • рекурсивно раскрываем все #include "…" (не системные);
 *      • используем каталоги, возвращаемые функцией
 *        `find_nonsystem_include_dirs(project_root)` – они уже содержат
 *        пути, указанные в системе сборки проекта;
 *      • **добавляем каталог исходного файла в список include‑директорий**;
 *      • сохраняем найденные файлы в один *.ctx‑файл (по порядку их
 *        первого появления);
 *      • выводим в консоль подробный отчёт:
 *            – сколько реально найдено файлов;
 *            – какие #include не удалось резолвить;
 *            – дерево зависимостей (как в include_tree.cpp).
 *
 *  Добавлена опция `-d N` – ограничение глубины включений заголовков.
 *********************************************************************/

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

#include "find_nonsys_includes.h"   // find_nonsystem_include_dirs

namespace fs = std::filesystem;

/* ------------------------------------------------------------------ *
 *  Вспомогательные функции
 * ------------------------------------------------------------------ */

/**
 * @brief Приводит путь к абсолютному (по возможности каноничному) виду.
 */
static fs::path resolve_to_absolute(const std::string& p,
                                    const fs::path& base)
{
    fs::path cur{p};
    if (!cur.is_absolute())
        cur = base / cur;
    try {
        return fs::weakly_canonical(cur);
    } catch (const fs::filesystem_error&) {
        return cur;   // fallback – оставляем как есть
    }
}

/* чтение целого файла в std::string */
static std::optional<std::string> read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    return content;
}

/* ------------------------------------------------------------------ *
 *  Поиск include‑ов
 * ------------------------------------------------------------------ */

/* поиск #include "…" в тексте (только кавычки) */
static std::vector<std::string> extract_quoted_includes(const std::string& src)
{
    static const std::regex inc_regex(
        R"re(\#\s*include\s*"([^"]+)")re",
        std::regex::optimize | std::regex::multiline);

    std::vector<std::string> result;
    std::smatch m;
    auto searchStart = src.cbegin();

    while (std::regex_search(searchStart, src.cend(), m, inc_regex)) {
        result.emplace_back(m[1].str());
        searchStart = m.suffix().first;
    }
    return result;
}

/* поиск #include <…> – системные заголовки (для отчёта) */
static std::vector<std::string> extract_angle_includes(const std::string& src)
{
    static const std::regex inc_regex(
        R"re(\#\s*include\s*<([^>]+)>)re",
        std::regex::optimize | std::regex::multiline);

    std::vector<std::string> result;
    std::smatch m;
    auto searchStart = src.cbegin();

    while (std::regex_search(searchStart, src.cend(), m, inc_regex)) {
        result.emplace_back(m[1].str());
        searchStart = m.suffix().first;
    }
    return result;
}

/* поиск заголовка в списке include‑директорий */
static std::optional<fs::path> locate_in_include_dirs(
        const std::string& header,
        const std::vector<std::string>& inc_dirs)
{
    for (const std::string& dir : inc_dirs) {
        fs::path p = fs::path(dir) / header;
        if (fs::exists(p) && fs::is_regular_file(p))
            return p;
    }
    return std::nullopt;
}

/* ------------------------------------------------------------------ *
 *  Структуры, собираемые «на лету»
 * ------------------------------------------------------------------ */
struct FileNode {
    std::string                 full_path;          // канонический абсолютный путь
    std::vector<std::string>    includes_raw;       // #include "…"
    std::vector<std::string>    includes_system;    // #include <…>
};

struct MissingInclude {
    std::string owner;      // файл, где написан #include
    std::string inc_text;   // как было записано в файле
};

/* ------------------------------------------------------------------ *
 *  Рекурсивный разбор одного файла
 * ------------------------------------------------------------------ */
static void process_file(
        const fs::path&                file_path,
        const std::vector<std::string>& include_dirs,
        std::unordered_map<std::string, FileNode>&   all_nodes,
        std::vector<fs::path>&         ordered_files,
        std::unordered_set<std::string>& processed,
        std::vector<MissingInclude>&   missing_includes,
        std::unordered_map<std::string,
                std::vector<std::string>>& dep_graph,
        int current_depth,          // <‑ добавлено
        int max_depth)              // <‑ добавлено
{
    /* 1. Канонизация пути */
    fs::path canonical;
    try {
        canonical = fs::weakly_canonical(file_path);
    } catch (const fs::filesystem_error&) {
        canonical = fs::absolute(file_path);   // fallback
    }
    const std::string key = canonical.generic_string();

    /* 2. Если уже обработали – выходим */
    if (processed.find(key) != processed.end())
        return;

    /* 3. Проверяем глубину: если вышли за пределы, не добавляем в контекст и не продолжаем рекурсию */
    if (max_depth >= 0 && current_depth > max_depth)
        return;   // файл не попадает в ordered_files

    /* 4. Читаем файл */
    auto src_opt = read_file(canonical);
    if (!src_opt) {
        std::cerr << "Warning: cannot read file " << canonical << '\n';
        processed.insert(key);
        return;
    }
    const std::string& src = *src_opt;

    /* 5. Формируем узел */
    FileNode node;
    node.full_path       = key;
    node.includes_raw    = extract_quoted_includes(src);
    node.includes_system = extract_angle_includes(src);
    all_nodes.emplace(key, std::move(node));

    /* 6. Добавляем в порядок (только после того, как уверены,
          что файл действительно существует) */
    ordered_files.emplace_back(canonical);
    processed.insert(key);

    /* 7. Выводим все найденные include‑ы (для отладки) */
    std::cout << "\n[DEBUG] Includes в " << canonical << ":\n";
    if (node.includes_raw.empty() && node.includes_system.empty())
        std::cout << "   (none)\n";
    else {
        for (const auto& inc : node.includes_raw)
            std::cout << "   \" " << inc << "\" (quoted)\n";
        for (const auto& inc : node.includes_system)
            std::cout << "   < " << inc << "> (angle)\n";
    }

    /* 8. Обрабатываем каждый кавычечный #include */
    for (const std::string& inc : all_nodes.at(key).includes_raw) {
        bool found = false;
        fs::path resolved;

        std::cout << "   -> trying to resolve \"" << inc << "\"\n";

        /* 8.1. Сначала ищем рядом с текущим файлом */
        resolved = resolve_to_absolute(inc, canonical.parent_path());
        if (fs::exists(resolved) && fs::is_regular_file(resolved)) {
            found = true;
            std::cout << "        found near source: " << resolved << '\n';
        }

        /* 8.2. Если не нашли – ищем в пользовательских include‑директах */
        if (!found) {
            if (auto maybe = locate_in_include_dirs(inc, include_dirs)) {
                resolved = *maybe;
                found = true;
                std::cout << "        found in include dirs: " << resolved << '\n';
            }
        }

        if (found) {
            /* Канонизируем путь, чтобы в графе и в all_nodes использовался один и тот же ключ */
            try {
                resolved = fs::weakly_canonical(resolved);
            } catch (const fs::filesystem_error&) {
                resolved = fs::absolute(resolved);
            }
            const std::string child_key = resolved.generic_string();

            /* Рекурсивный вызов с увеличенным уровнем */
            process_file(resolved,
                         include_dirs,
                         all_nodes,
                         ordered_files,
                         processed,
                         missing_includes,
                         dep_graph,
                         current_depth + 1,   // <‑ новый уровень
                         max_depth);          // <‑ тот же лимит

            dep_graph[key].push_back(child_key);
        } else {
            std::cout << "        NOT FOUND\n";
            missing_includes.push_back({key, inc});
        }
    }
}

/* ------------------------------------------------------------------ *
 *  Вывод дерева зависимостей (улучшенный)
 * ------------------------------------------------------------------ */
static void print_dependency_tree(
        const std::unordered_map<std::string, std::vector<std::string>>& dep_graph,
        const std::unordered_set<std::string>& all_node_keys)
{
    std::cout << "\n=== ДЕРЕВО ЗАВИСИМОСТЕЙ ===\n";

    /* 1. Находим корневые узлы (не являющиеся дочерними) */
    std::unordered_set<std::string> child_set;
    for (const auto& [parent, children] : dep_graph)
        for (const auto& ch : children)
            child_set.insert(ch);

    std::vector<std::string> roots;
    for (const auto& key : all_node_keys)
        if (!child_set.count(key))
            roots.push_back(key);

    if (roots.empty()) {
        std::cout << "(no files were processed)\n";
        return;
    }

    /* 2. Рекурсивный вывод с красивыми префиксами */
    std::function<void(const std::string&, const std::string&, bool)> dfs =
        [&](const std::string& node,
            const std::string& prefix,
            bool is_last)
    {
        std::cout << prefix;
        if (!prefix.empty())
            std::cout << (is_last ? "└─ " : "├─ ");

        std::cout << fs::path(node).filename().string() << '\n';

        auto it = dep_graph.find(node);
        if (it == dep_graph.end())
            return;

        const auto& children = it->second;
        for (size_t i = 0; i < children.size(); ++i) {
            bool child_last = (i + 1 == children.size());
            std::string child_prefix = prefix + (is_last ? "   " : "│  ");
            dfs(children[i], child_prefix, child_last);
        }
    };

    for (size_t i = 0; i < roots.size(); ++i) {
        bool last_root = (i + 1 == roots.size());
        dfs(roots[i], "", last_root);
    }
}

/* ------------------------------------------------------------------ *
 *  Запись .ctx‑файла
 * ------------------------------------------------------------------ */
static void write_context(const fs::path& source_path,
                          const std::vector<fs::path>& ordered_files)
{
    fs::path ctx_path = source_path.parent_path() /
                        (source_path.stem().string() + ".ctx");

    std::ofstream out(ctx_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: cannot create context file " << ctx_path << '\n';
        return;
    }

    for (const fs::path& p : ordered_files) {
        out << "// " << p.generic_string() << "\n";
        if (auto src = read_file(p))
            out << *src << "\n";
        else
            out << "// *** FAILED TO READ FILE ***\n";

        out << "\n";   // разделитель между файлами
    }

    std::cout << "Context written to " << ctx_path << '\n';
}

/* ------------------------------------------------------------------ *
 *  Главная функция
 * ------------------------------------------------------------------ */
int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " [-d N] <cpp|h|hpp> [<cpp|h|hpp> ...]\n";
        return 1;
    }

    /* ---------- 1. Парсим опцию глубины -------------------------------- */
    int max_depth = -1;                     // по умолчанию – без ограничения
    std::vector<std::string> files_to_process;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-d" || arg == "--depth") && i + 1 < argc) {
            try {
                max_depth = std::stoi(argv[++i]);   // увеличиваем индекс, чтобы пропустить число
                if (max_depth < 0) throw std::invalid_argument("negative");
            } catch (...) {
                std::cerr << "Error: invalid depth value after "
                          << arg << '\n';
                return 1;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << '\n';
            return 1;
        } else {
            files_to_process.push_back(arg);
        }
    }

    if (files_to_process.empty()) {
        std::cerr << "Error: no source files specified.\n";
        return 1;
    }

    const fs::path program_cwd = fs::current_path();

    for (const auto& raw_input : files_to_process) {
        /* ---------- 2. Подготовка пути к исходному файлу ---------------- */
        const fs::path source_abs = resolve_to_absolute(raw_input, program_cwd);

        if (!fs::exists(source_abs) || !fs::is_regular_file(source_abs)) {
            std::cerr << "Error: file not found or not regular: "
                      << source_abs << '\n';
            continue;
        }

        const fs::path project_root = source_abs.parent_path();

        /* ---------- 3. Формируем список include‑директорий ---------------- */
        std::vector<std::string> include_dirs;
        include_dirs.emplace_back(project_root.string());   // всегда первый

        if (auto maybe_dirs = find_nonsystem_include_dirs(project_root)) {
            for (const std::string& d : *maybe_dirs)
                include_dirs.emplace_back(d);
        }

        /* ---------- 4. Структуры для сбора данных ----------------------- */
        std::unordered_map<std::string, FileNode> all_nodes;
        std::vector<fs::path> ordered_files;
        std::unordered_set<std::string> processed;
        std::vector<MissingInclude> missing_includes;
        std::unordered_map<std::string, std::vector<std::string>> dep_graph;

        /* ---------- 5. Рекурсивный разбор с учётом глубины ---------------- */
        process_file(source_abs,
                     include_dirs,
                     all_nodes,
                     ordered_files,
                     processed,
                     missing_includes,
                     dep_graph,
                     0,          // current_depth
                     max_depth); // max_depth

        /* ---------- 6. Отчёт --------------------------------------------- */
        std::cout << "\n=== ОТЧЁТ ДЛЯ " << source_abs << " ===\n";
        std::cout << "Глубина включений: "
                  << (max_depth >= 0 ? std::to_string(max_depth) : "без ограничения")
                  << '\n';

        std::cout << "\nНайдено файлов (реально существующих): "
                  << ordered_files.size() << '\n';

        /* --- Кавычечные include‑ы --- */
        if (!missing_includes.empty()) {
            std::cout << "\nНе найденные #include \"…\":\n";
            for (const auto& miss : missing_includes) {
                std::cout << "  [" << miss.owner << "] -> \""
                          << miss.inc_text << "\"\n";
            }
        } else {
            bool any_quoted = false;
            for (const auto& kv : all_nodes)
                if (!kv.second.includes_raw.empty())
                    any_quoted = true;
            if (any_quoted) {
                std::cout << "\nВсе кавычечные #include успешно резолвились.\n";
            } else {
                std::cout << "\nПочему-то заголовочные файлы не найдены.\n";
            }
        }

        /* --- Системные include‑ы (только количество) --- */
        std::size_t sys_cnt = 0;
        for (const auto& [_, node] : all_nodes)
            sys_cnt += node.includes_system.size();
        if (sys_cnt)
            std::cout << "\nОбнаружено " << sys_cnt
                      << " системных заголовков (не включаются в .ctx).\n";

        /* ---------- 7. Вывод дерева зависимостей ------------------------ */
        std::unordered_set<std::string> node_keys;
        for (const auto& kv : all_nodes)
            node_keys.insert(kv.first);

        print_dependency_tree(dep_graph, node_keys);

        /* ---------- 8. Запись .ctx‑файла --------------------------------- */
        write_context(source_abs, ordered_files);
    }

    return 0;
}

