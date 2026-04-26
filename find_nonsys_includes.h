#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

/**
 * @brief Возвращает список абсолютных путей «не‑системных» каталогов включения,
 * найденных в проекте, описанном конфигурационными файлами (Makefile, CMakeLists.txt,
 * *.pro). Если ни один такой каталог не обнаружен – возвращается std::nullopt.
 *
 * @param project_root Корень проекта
 * @return std::optional<std::vector<std::string>> Список путей или std::nullopt
 */
std::optional<std::vector<std::string>>
find_nonsystem_include_dirs(const fs::path& project_root);

