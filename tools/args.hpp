// Minimal --key value / --flag command-line parsing shared by the tools.
#pragma once

#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

struct Args {
    std::map<std::string, std::string> kv;
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            if (k.rfind("--", 0) != 0) throw std::invalid_argument("unexpected argument: " + k);
            k = k.substr(2);
            if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) kv[k] = argv[++i];
            else kv[k] = "1";
        }
    }
    bool has(const std::string& k) const { return kv.count(k) > 0; }
    std::string str(const std::string& k, const std::string& def) const { return has(k) ? kv.at(k) : def; }
    double num(const std::string& k, double def) const { return has(k) ? std::strtod(kv.at(k).c_str(), nullptr) : def; }
};
