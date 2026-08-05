#include "codegraph/cli_options.hpp"

#include <sstream>
#include <stdexcept>

namespace codegraph {

// возвращает текст справки по использованию программы, включая имя программы
std::string usage_text(const char* program_name) {
    std::ostringstream oss;
    oss << "usage: " << program_name << " <project_root> [--out <file>] [--ignore <name>]...\n"
        << "\n"
        << "exit codes: 0 success, 1 filesystem/runtime error, 2 usage error";
    return oss.str();
}

// разбирает аргументы командной строки и возвращает структуру CliOptions
CliOptions parse_cli_options(int argc, char** argv) {
    CliOptions options;
    bool have_root = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--out") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("--out requires a file path\n" +
                                             usage_text(argv[0]));
            }
            options.out_file = argv[++i];
        } else if (arg == "--ignore") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("--ignore requires a name\n" + usage_text(argv[0]));
            }
            options.extra_ignores.push_back(argv[++i]);
        } else if (!have_root) {
            options.project_root = arg;
            have_root = true;
        } else {
            throw std::invalid_argument("unexpected argument: " + arg + "\n" +
                                         usage_text(argv[0]));
        }
    }

    if (!have_root) {
        throw std::invalid_argument("missing required <project_root>\n" + usage_text(argv[0]));
    }

    return options;
}

}  
