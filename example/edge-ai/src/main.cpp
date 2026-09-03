#include "pipeline_manager.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view APP_VERSION   = "0.0.4";
constexpr std::string_view BUILD_DATE    = __DATE__;
constexpr std::string_view BUILD_TIME    = __TIME__;

void signal_handler(int signal_number)
{
    std::_Exit(128 + signal_number);
}

void print_version()
{
    std::cout << "edge-ai v" << APP_VERSION
              << " (built " << BUILD_DATE << " " << BUILD_TIME << ")\n";
}

void setup_signal_handlers()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

void print_usage(std::string_view program)
{
    std::cout
        << "Usage:\n"
        << "  " << program << " <pipeline.json>           Run a JSON pipeline\n"
        << "  " << program << " <pipeline.json> --stream  Stream raw PCM from stdin continuously\n"
        << "  " << program << " <pipeline.json> --debug   Enable per-batch logs\n"
        << "  " << program << " --preload                 Load default model into C7x (run at boot)\n"
        << "  " << program << " --version                 Show version and build info\n"
        << "  " << program << " --help                    Show this help\n\n"
        << "Examples:\n"
        << "  " << program << " pipeline_tvm_inference.json\n"
        << "  arecord -t raw -f S16_LE -c 1 -r 16000 | " << program
        << " pipeline_speech_classification_yamnet.json --stream\n";
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        std::string json_file;
        bool debug   = false;
        bool preload = false;
        bool stream  = false;

        for (int index = 1; index < argc; ++index) {
            const std::string_view argument{argv[index]};
            if (argument == "--help" || argument == "-h") {
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            }
            if (argument == "--version" || argument == "-v") {
                print_version();
                return EXIT_SUCCESS;
            }
            if (argument == "--debug" || argument == "-d") {
                debug = true;
                continue;
            }
            if (argument == "--preload") {
                preload = true;
                continue;
            }
            if (argument == "--stream") {
                stream = true;
                continue;
            }
            if (argument.rfind("--", 0) == 0) {
                std::cerr << "[App] Unknown argument: " << argument << '\n';
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            if (!json_file.empty()) {
                std::cerr << "[App] Only one pipeline file may be specified\n";
                return EXIT_FAILURE;
            }
            json_file = argument;
        }

        if (!preload && json_file.empty()) {
            std::cerr << "[App] Error: A pipeline JSON file is required\n";
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }

        setup_signal_handlers();
        print_version();
        std::cout << "===========================================\n"
                     "       RPMsg Inference Example\n"
                     "===========================================\n\n";

        PipelineManager application;
        application.set_debug(debug);

        if (preload)
            return application.preload_default_model();

        const int exit_code = stream
            ? application.run_from_json_file_stream(json_file)
            : application.run_from_json_file(json_file);
        std::cout << "[App] Application exited with code " << exit_code << '\n';
        return exit_code;
    } catch (const std::exception& error) {
        std::cerr << "[App] Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
