#include "Runtime.hpp"

#include <iostream>

namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kGreen = "\033[32m";
constexpr const char* kCyan = "\033[36m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kRed = "\033[31m";

} // namespace

void Runtime::register_function(std::string name, JsonFn f) {
    const std::string label = name;
    std::lock_guard<std::mutex> lock(mutex_);
    functions_[std::move(name)] = std::move(f);
    std::cerr << kCyan << "[mlxf] " << kReset << "Registered function '" << label << "'"
              << std::endl;
}

nlohmann::json Runtime::call(const std::string& name, const nlohmann::json& args) {
    JsonFn fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = functions_.find(name);
        if (it == functions_.end()) {
            std::cerr << kRed << "[mlxf] " << kReset << "No such function: '" << name << "'"
                      << std::endl;
            return nlohmann::json{{"error", "No such function: " + name}};
        }
        fn = it->second;
    }
    try {
        nlohmann::json result = fn(args);
        if (result.contains("__error__") && result["__error__"].get<bool>()) {
            std::cerr << kYellow << "[mlxf] " << kReset << "Function '" << name
                      << "' reported error: " << result.value("message", "") << std::endl;
        }
        return result;
    } catch (const std::exception& ex) {
        std::cerr << kRed << "[mlxf] " << kReset << "Exception in call('" << name
                  << "'): " << ex.what() << std::endl;
        return nlohmann::json{{"__error__", true}, {"message", ex.what()}};
    } catch (...) {
        std::cerr << kRed << "[mlxf] " << kReset << "Unknown exception in call('" << name
                  << "')" << std::endl;
        return nlohmann::json{{"__error__", true}, {"message", "unknown exception"}};
    }
}

bool Runtime::has_function(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return functions_.find(name) != functions_.end();
}

void Runtime::clear_functions() {
    std::lock_guard<std::mutex> lock(mutex_);
    functions_.clear();
}
