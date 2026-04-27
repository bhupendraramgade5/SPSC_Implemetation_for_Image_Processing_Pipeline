#ifndef CONFIG_MANAGER_HPP
#define CONFIG_MANAGER_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <iostream> 

// Mode Enum
enum class Mode {
    RANDOM,
    CSV
};

inline std::ostream& operator<<(std::ostream& os, Mode m) {
    return os << (m == Mode::CSV ? "CSV" : "RANDOM");
}


struct SystemConfig {
    // m : Required Input   (Matrix Size)
    size_t      columns       = 0;       
    // T : Required Input       (Time in seconds)
    uint64_t    cycle_time_ns = 0;      
     // TV : Required Input  (Threshold Value) 
    uint8_t     threshold     = 0;       
    std::vector<float> kernel;           
    Mode        mode          = Mode::RANDOM;
    std::string input_file;
    uint64_t run_duration_ms = 0;
    uint64_t max_rows        = 0;
};

// -----------------------------
// Config Manager

class ConfigManager {
public:
    static SystemConfig load(int argc, char** argv);

private:
    static SystemConfig parseFile(const std::string& path);
    static void overrideWithCLI(SystemConfig& config, int argc, char** argv);
    static void validate(SystemConfig& config);

    static std::vector<float> defaultKernel();
    static Mode parseMode(const std::string& str);
};

#endif // CONFIG_MANAGER_HPP