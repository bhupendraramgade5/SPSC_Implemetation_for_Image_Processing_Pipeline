#include "ConfigManager.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t");
    size_t end = s.find_last_not_of(" \t");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

SystemConfig ConfigManager::load(int argc, char** argv) {
    SystemConfig config = parseFile("config.cfg");

    overrideWithCLI(config, argc, argv);

    validate(config);

    return config;
}

SystemConfig ConfigManager::parseFile(const std::string& path) {
    SystemConfig config;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: config file not found, using defaults\n";
        return config;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));

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
    }

    return config;
}

void ConfigManager::overrideWithCLI(SystemConfig& config, int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--mode=csv") {
            config.mode = Mode::CSV;
        } else if (arg == "--mode=random") {
            config.mode = Mode::RANDOM;
        }
        // Extend as needed
    }
}

void ConfigManager::validate(SystemConfig& config) {
    if (config.columns == 0) {
        throw std::runtime_error("Invalid config: columns (m) must be > 0");
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