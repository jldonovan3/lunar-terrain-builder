#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "builder/builder.hpp"

int main(int argc, char** argv) {
    CLI::App app{"Build and inspect deterministic lunar terrain databases"};
    app.set_version_flag(
        "--version", fmt::format("lunar-terrain {}", lunar::terrain::builder::version_string()));
    CLI11_PARSE(app, argc, argv);
    return 0;
}
