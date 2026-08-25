#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "builder/builder.hpp"

namespace {

[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string encoded{"\""};
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            encoded.push_back('\\');
        }
        if (static_cast<unsigned char>(character) >= 0x20U) {
            encoded.push_back(character);
        }
    }
    encoded.push_back('"');
    return encoded;
}

[[nodiscard]] int report_error(const lunar::terrain::Error& error, const bool json) {
    if (json) {
        fmt::print(
            stderr,
            "{{\"code\":{},\"message\":{},\"path\":{}}}\n",
            json_string(lunar::terrain::error_code_name(error.code)),
            json_string(error.message),
            json_string(error.context.path));
    } else {
        fmt::print(stderr, "{}: {}", lunar::terrain::error_code_name(error.code), error.message);
        if (!error.context.path.empty()) {
            fmt::print(stderr, " [{}]", error.context.path);
        }
        if (error.context.tile_key) {
            fmt::print(stderr, " [tile={:016x}]", *error.context.tile_key);
        }
        if (error.context.channel_id) {
            fmt::print(stderr, " [channel={}]", *error.context.channel_id);
        }
        fmt::print(stderr, "\n");
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Build and inspect deterministic lunar terrain databases"};
    app.set_version_flag(
        "--version", fmt::format("lunar-terrain {}", lunar::terrain::builder::version_string()));

    std::filesystem::path scan_path;
    bool scan_json = false;
    CLI::App* scan = app.add_subcommand("scan", "Catalog and validate a builder configuration and source");
    scan->add_option("configuration", scan_path, "TOML configuration path")->required();
    scan->add_flag("--json", scan_json, "Emit machine-readable JSON");

    std::filesystem::path plan_path;
    bool plan_json = false;
    CLI::App* plan = app.add_subcommand("plan", "Plan the configured prototype build");
    plan->add_option("configuration", plan_path, "TOML configuration path")->required();
    plan->add_flag("--json", plan_json, "Emit machine-readable JSON");

    std::filesystem::path build_path;
    bool build_json = false;
    CLI::App* build = app.add_subcommand("build", "Build and transactionally publish LTDB/LTP files");
    build->add_option("configuration", build_path, "TOML configuration path")->required();
    build->add_flag("--json", build_json, "Emit machine-readable JSON");

    std::filesystem::path validate_path;
    bool validate_full = false;
    bool validate_json = false;
    CLI::App* validate = app.add_subcommand("validate", "Validate an LTDB and its referenced packs");
    validate->add_option("database", validate_path, "LTDB manifest path")->required();
    validate->add_flag("--full", validate_full, "Also verify every same-level seam");
    validate->add_flag("--json", validate_json, "Emit machine-readable JSON");

    std::filesystem::path inspect_path;
    std::string inspect_key;
    bool inspect_json = false;
    CLI::App* inspect = app.add_subcommand("inspect", "Inspect an LTDB or one tile");
    inspect->add_option("database", inspect_path, "LTDB manifest path")->required();
    inspect->add_option("tile", inspect_key, "Optional canonical LunarTileKey");
    inspect->add_flag("--json", inspect_json, "Emit machine-readable JSON");

    std::filesystem::path export_path;
    std::filesystem::path export_output;
    std::string export_key;
    std::string export_format;
    CLI::App* export_command = app.add_subcommand(
        "export", "Export M5 provenance, quality, or transition diagnostics");
    export_command->add_option("database", export_path, "LTDB manifest path")->required();
    export_command->add_option("tile", export_key, "Canonical LunarTileKey")->required();
    export_command->add_option(
        "--format", export_format,
        "provenance-ppm, quality-ppm, or transition-csv")->required();
    export_command->add_option("--output", export_output, "Diagnostic output path")->required();

    app.require_subcommand(1);
    CLI11_PARSE(app, argc, argv);

    using namespace lunar::terrain::builder;
    if (*scan) {
        auto configuration = load_configuration(scan_path);
        if (!configuration) {
            return report_error(configuration.error(), scan_json);
        }
        auto report = scan_configuration(configuration.value());
        if (!report) {
            return report_error(report.error(), scan_json);
        }
        fmt::print("{}", format_report(report.value(), scan_json));
        return 0;
    }
    if (*plan) {
        auto configuration = load_configuration(plan_path);
        if (!configuration) {
            return report_error(configuration.error(), plan_json);
        }
        auto report = plan_configuration(configuration.value());
        if (!report) {
            return report_error(report.error(), plan_json);
        }
        fmt::print("{}", format_report(report.value(), plan_json));
        return 0;
    }
    if (*build) {
        auto configuration = load_configuration(build_path);
        if (!configuration) {
            return report_error(configuration.error(), build_json);
        }
        auto report = build_configuration(configuration.value());
        if (!report) {
            return report_error(report.error(), build_json);
        }
        fmt::print("{}", format_report(report.value(), build_json));
        return 0;
    }
    if (*validate) {
        auto report = validate_database(validate_path, validate_full);
        if (!report) {
            return report_error(report.error(), validate_json);
        }
        fmt::print("{}", format_report(report.value(), validate_json));
        return 0;
    }
    if (*inspect) {
        std::optional<lunar::terrain::LunarTileKey> key;
        if (!inspect_key.empty()) {
            auto parsed = lunar::terrain::LunarTileKey::parse(inspect_key);
            if (!parsed) {
                return report_error(parsed.error(), inspect_json);
            }
            key = parsed.value();
        }
        auto report = inspect_database(inspect_path, key);
        if (!report) {
            return report_error(report.error(), inspect_json);
        }
        fmt::print("{}", format_report(report.value(), inspect_json));
        return 0;
    }
    if (*export_command) {
        auto key = lunar::terrain::LunarTileKey::parse(export_key);
        if (!key) {
            return report_error(key.error(), false);
        }
        std::optional<DiagnosticExportFormat> format;
        if (export_format == "provenance-ppm") {
            format = DiagnosticExportFormat::provenance_ppm;
        } else if (export_format == "quality-ppm") {
            format = DiagnosticExportFormat::quality_ppm;
        } else if (export_format == "transition-csv") {
            format = DiagnosticExportFormat::transition_csv;
        }
        if (!format) {
            return report_error(lunar::terrain::Error{
                lunar::terrain::ErrorCode::invalid_argument,
                "export format must be provenance-ppm, quality-ppm, or transition-csv"}, false);
        }
        auto exported = export_tile_diagnostic(
            export_path, key.value(), *format, export_output);
        if (!exported) {
            return report_error(exported.error(), false);
        }
        fmt::print("exported {}\n", export_output.string());
        return 0;
    }
    return 1;
}
