// Standalone log generator for fixtures and Stream Mode smoke tests. Option
// handling lives in `options.cpp`, the generation loop in `generator.cpp`.

#include <log_generator/generator.hpp>
#include <log_generator/options.hpp>

#include <CLI/CLI.hpp>

#include <exception>
#include <iostream>

// CLI11 reports usage errors through exceptions by design.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[])
{
    CLI::App app(
        "Generate synthetic log records (timestamp, level, component, message, ...) as a file, on stdout, or over "
        "the network.",
        "log_generator"
    );
    app.set_version_flag("--version", "1.0.0");
    app.get_formatter()->enable_footer_formatting(false);
    app.footer(
        "Examples:\n"
        "  log_generator -n 100k -o app.jsonl                    100,000 JSON lines as fast as possible\n"
        "  log_generator -f syslog --rate 50 --forever -o -      endless syslog stream on stdout\n"
        "  log_generator --start-time 2026-01-01 -d 1d --rate 5 --bursts -o day.jsonl\n"
        "                                                        one simulated day, written instantly\n"
        "  log_generator --rate 20 --roll-size 1M -o app.log     live file rotated every MiB\n"
        "  log_generator -o tcp://127.0.0.1:5141 -n 1000 --rate 200\n"
        "\n"
        "Every record has a 0-based line_number that continues across rotations."
    );

    log_generator::GeneratorOptions options;
    log_generator::AddOptions(app, options);
    try
    {
        app.parse(argc, argv);
    }
    catch (const CLI::ParseError &error)
    {
        return app.exit(error) == 0 ? 0 : 1;
    }

    try
    {
        log_generator::Run(options);
    }
    catch (const std::exception &err)
    {
        std::cerr << "log_generator: " << err.what() << '\n';
        return 1;
    }
    return 0;
}
