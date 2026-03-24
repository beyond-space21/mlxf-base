#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

/// Central registry for polyglot callbacks. All cross-language calls use JSON
/// for arguments and return values.
class Runtime {
public:
    using JsonFn = std::function<nlohmann::json(const nlohmann::json&)>;

    void register_function(std::string name, JsonFn f);
    nlohmann::json call(const std::string& name,
                          const nlohmann::json& args = nlohmann::json::object());

    bool has_function(const std::string& name) const;

    /// Drop all registered callbacks. Call before destroying the V8 isolate when any
    /// callback holds v8::Global handles (e.g. JS-registered functions).
    void clear_functions();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, JsonFn> functions_;
};
