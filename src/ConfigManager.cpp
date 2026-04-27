#include "ConfigManager.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <stdexcept>


static std::string trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    const size_t end   = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

SystemConfig ConfigManager::load(int argc, char** argv) {
    
    std::string cfg_path = "config.cfg";
    bool explicit_path = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // --config=<path> style
        if (arg.rfind("--config=", 0) == 0) {
            cfg_path = arg.substr(9);
            explicit_path = true;
            break;
        }
    }
    std::cout << "[ConfigManager] Config Path : " << cfg_path << '\n';

    std::ifstream probe(cfg_path);
    if (!probe.is_open()) {
        if (explicit_path) {
            throw std::runtime_error(
                "Config file not found: '" + cfg_path + "'");
        }
        std::cerr << "[ConfigManager] Warning: '" << cfg_path
                  << "' not found, using compiled-in defaults + CLI args.\n";
    }
    probe.close();

    SystemConfig config = parseFile(cfg_path);
    overrideWithCLI(config, argc, argv);

    validate(config);

    return config;
}

SystemConfig ConfigManager::parseFile(const std::string& path) {
    SystemConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: config file not found, using defaults\n";
        std::cout<<"Path : "<<path<<std::endl;
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        const std::string key   = trim(line.substr(0, pos));
        const std::string value = trim(line.substr(pos + 1));

        if (key == "m") {
            config.columns = std::stoul(value);
        } else if (key == "T") {
            config.cycle_time_ns = std::stoull(value);
        } else if (key == "threshold") {
            config.threshold = static_cast<uint8_t>(std::stoi(value));
        } else if (key == "mode") {
            config.mode = parseMode(value);
        } else if (key == "input_file") {
            config.input_file = value;
        } else if (key == "kernel") {
            std::stringstream ss(value);
            std::string num;
            while (std::getline(ss, num, ',')) {
                config.kernel.push_back(std::stof(trim(num)));
            }
        }
        // -- New: termination controls ----------------------------------------
        else if (key == "run_duration_ms") {
            config.run_duration_ms = std::stoull(value);
        }
        else if (key == "max_rows") {
            config.max_rows = std::stoull(value);
        }
        // unknown keys: silently ignored (forward-compatibility)
    }

    return config;
}

void ConfigManager::overrideWithCLI(SystemConfig& config, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        // Skip bare positional arguments (the config file path).
        if (arg.rfind("--", 0) != 0) continue;

        if (arg == "--mode=csv") {
            config.mode = Mode::CSV;
        } else if (arg == "--mode=random") {
            config.mode = Mode::RANDOM;
        }
        else if (arg.rfind("--duration=", 0) == 0) {
            config.run_duration_ms = std::stoull(arg.substr(11));
        }
        else if (arg.rfind("--max-rows=", 0) == 0) {
            config.max_rows = std::stoull(arg.substr(11));
        }
        // --config=<path> is consumed in load() — ignore here
        // unknown flags: silently ignored
    }
}

void ConfigManager::validate(SystemConfig& config) {
    if (config.columns == 0) {
        throw std::runtime_error("Invalid config: columns (m) must be > 0");
    }
    
    if (config.columns < 2) {
        throw std::runtime_error(
            "Invalid config: columns (m) must be >= 2 "
            "(pipeline outputs 2 consecutive pixels per cycle)");
    }

    if (config.columns % 2 != 0) {
        throw std::runtime_error(
            "Invalid config: columns (m) must be even "
            "(pipeline outputs 2 consecutive pixels per cycle, got m="
            + std::to_string(config.columns) + ")");
    }

    if (config.cycle_time_ns == 0) {
        throw std::runtime_error("Invalid config: T must be > 0");
    }

    if (config.kernel.empty()) {
        config.kernel = defaultKernel();
    }

    if (config.kernel.size() % 2 == 0) {
        throw std::runtime_error("Kernel size must be odd");
    }

    if (config.mode == Mode::CSV && config.input_file.empty()) {
        throw std::runtime_error(
            "Invalid config: CSV mode requires input_file to be set");
    }

    // run_duration_ms / max_rows: 0 = unlimited; any non-negative value OK.
}

std::vector<float> ConfigManager::defaultKernel() {
    return {
        0.00025177f,
        0.008666992f,
        0.078025818f,
        0.24130249f,
        0.343757629f,
        0.24130249f,
        0.078025818f,
        0.008666992f,
        0.000125885f
    };
}

Mode ConfigManager::parseMode(const std::string& str) {
    if (str == "csv") return Mode::CSV;
    return Mode::RANDOM;
}