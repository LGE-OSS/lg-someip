/********************************************************************************
 * Copyright (C) 2026 LG Electronics Inc.
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ********************************************************************************/

#ifndef LG_SOMEIP_PERF_OPTIONS_H
#define LG_SOMEIP_PERF_OPTIONS_H

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>

namespace perf {

// Minimal "--key=value" / "--key value" / "--flag" parser. Unknown keys are
// rejected so a typo cannot silently fall back to a default value.
class Options {
public:
    Options(int argc, char* argv[], const std::map<std::string, std::string>& defaults,
            const std::set<std::string>& flags = {})
        : values_(defaults), flags_(flags) {
        for (int i = 1; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "-h" || arg == "--help") {
                help_ = true;
                continue;
            }
            if (arg.compare(0, 2, "--") != 0) {
                fail("unexpected argument '" + arg + "'");
                continue;
            }
            arg = arg.substr(2);
            std::string value;
            auto eq = arg.find('=');
            if (eq != std::string::npos) {
                value = arg.substr(eq + 1);
                arg = arg.substr(0, eq);
            } else if (flags_.count(arg) != 0) {
                value = "1";
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                fail("missing value for --" + arg);
                continue;
            }
            if (values_.count(arg) == 0 && flags_.count(arg) == 0) {
                fail("unknown option --" + arg);
                continue;
            }
            values_[arg] = value;
        }
    }

    bool ok() const {
        return ok_ && !help_;
    }

    void print_usage(const char* program) const {
        std::cout << "Usage: " << program << " [options]\n";
        for (const auto& flag : flags_) {
            std::cout << "  --" << flag << "\n";
        }
        for (const auto& entry : values_) {
            if (flags_.count(entry.first) == 0) {
                std::cout << "  --" << entry.first << " <value>  (default: '" << entry.second << "')\n";
            }
        }
    }

    std::string str(const std::string& key) const {
        auto it = values_.find(key);
        return it == values_.end() ? std::string() : it->second;
    }

    // Decimal, non-negative integer. Anything else (a negative number, a
    // trailing letter, an empty value) ends the program: a wrapped-around or
    // silently truncated count would make a benchmark run something else than
    // what was asked for.
    std::uint64_t u64(const std::string& key) const {
        const std::string text = str(key);
        char* end = nullptr;
        errno = 0;
        const auto value = std::strtoull(text.c_str(), &end, 10);
        if (text.empty() || text[0] == '-' || end == text.c_str() || *end != '\0' || errno == ERANGE) {
            std::cerr << "error: --" << key << " expects a non-negative decimal integer, got '" << text << "'"
                      << std::endl;
            std::exit(1);
        }
        return value;
    }

    bool flag(const std::string& key) const {
        auto value = str(key);
        return !value.empty() && value != "0" && value != "false";
    }

private:
    void fail(const std::string& reason) {
        std::cerr << "error: " << reason << std::endl;
        ok_ = false;
    }

    std::map<std::string, std::string> values_;
    std::set<std::string> flags_;
    bool ok_{true};
    bool help_{false};
};

} // namespace perf

#endif // LG_SOMEIP_PERF_OPTIONS_H
