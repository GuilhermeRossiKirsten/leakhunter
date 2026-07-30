/// @file main.cpp
/// @brief Entry point: parse, configure logging, delegate to Application.

#include <exception>
#include <iostream>
#include <memory>

#include "leakhunter/app/Application.hpp"
#include "leakhunter/cli/CommandLineParser.hpp"
#include "leakhunter/core/Logger.hpp"
#include "leakhunter/process/PosixProcessRunner.hpp"

int main(int argc, char** argv) {
    using namespace leakhunter;

    try {
        const cli::CommandLineParser parser(std::cout);

        auto options = parser.parse(argc, argv);
        if (!options) {
            std::cerr << "leakhunter: " << options.message() << '\n';
            return static_cast<int>(app::ExitCode::UsageError);
        }
        if (options.value().shouldExitEarly) {
            return options.value().earlyExitCode;
        }

        log::initialize(options.value().verbosity);

        app::Application application(std::make_unique<process::PosixProcessRunner>(), std::cout,
                                     std::cerr);
        return static_cast<int>(application.run(options.value()));
    } catch (const std::exception& error) {
        std::cerr << "leakhunter: unexpected failure: " << error.what() << '\n';
        return static_cast<int>(app::ExitCode::InternalError);
    } catch (...) {
        std::cerr << "leakhunter: unexpected failure\n";
        return static_cast<int>(app::ExitCode::InternalError);
    }
}
