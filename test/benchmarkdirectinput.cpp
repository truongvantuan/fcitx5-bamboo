/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "bamboo.h"
#include <chrono>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace fcitx;

long peakResidentKiB() {
    // /proc tracks this executable's address space, excluding the launcher.
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 6, "VmHWM:") == 0) {
            return std::stol(line.substr(6));
        }
    }
    return 0;
}

// Measure engine CPU work, without GUI scheduling or a client's text indexing.
// The document cursor stays at the end and edits are immediate; the engine
// maintains its own surrounding-text cache, as it does before client echoes.
class BenchmarkClient final : public InputContext {
public:
    BenchmarkClient(Instance &instance, bool zen, size_t prefixBytes)
        : InputContext(instance.inputContextManager(),
                       zen ? "zen.desktop" : "benchmark") {
        text_.reserve(prefixBytes + 10000);
        while (text_.size() < prefixBytes) {
            text_ += "Việt ";
        }
        cursor_ = utf8::length(text_);
        created();
        setCapabilityFlags(CapabilityFlags(CapabilityFlag::SurroundingText) |
                           CapabilityFlag::Preedit);
        focusIn();
        surroundingText().setText(text_, cursor_, cursor_);
    }
    ~BenchmarkClient() override { destroy(); }
    const char *frontend() const override { return "ibus"; }

    void type(BambooEngine &engine, const InputMethodEntry &entry,
              std::string_view keys) {
        for (const char key : keys) {
            KeyEvent event(this, Key(static_cast<KeySym>(
                                     static_cast<unsigned char>(key))));
            engine.keyEvent(entry, event);
            if (!event.accepted()) {
                throw std::runtime_error("Benchmark key was not accepted");
            }
        }
    }

private:
    void commitStringImpl(const std::string &value) override {
        text_.append(value);
        cursor_ += utf8::length(value);
    }
    void deleteSurroundingTextImpl(int offset, unsigned int size) override {
        if (offset != -static_cast<int>(size) || size > cursor_) {
            throw std::runtime_error("Benchmark deletion was not a suffix");
        }
        while (size--) {
            auto begin = text_.size() - 1;
            while ((static_cast<unsigned char>(text_[begin]) & 0xc0) == 0x80) {
                --begin;
            }
            text_.resize(begin);
            --cursor_;
        }
    }
    void forwardKeyImpl(const ForwardKeyEvent &) override {
        throw std::runtime_error("Reliable benchmark client received a key");
    }
    void updatePreeditImpl() override {}

    std::string text_;
    unsigned int cursor_ = 0;
};

int main(int argc, char **argv) {
    try {
        const std::string_view filter = argc > 1 ? argv[1] : "all";
        setupTestingEnvironment(TEST_DATA_DIR, {}, {TEST_DATA_DIR});
        char arg0[] = "benchmarkdirectinput";
        char arg1[] = "--disable=all";
        char *instanceArgs[] = {arg0, arg1};
        Instance instance(2, instanceArgs);
        instance.addonManager().registerDefaultLoader(nullptr);
        instance.initialize();
        BambooEngine engine(&instance);
        const InputMethodEntry entry("bamboo", "Bamboo", "vi", "bamboo");
        RawConfig config;
        config.setValueByPath("DirectInput", "True");
        config.setValueByPath("SpellCheck", "False");
        config.setValueByPath("Macro", "False");
        engine.setConfig(config);

        struct Case {
            const char *name;
            bool zen;
            size_t prefixBytes;
            size_t words;
            std::string keys;
        };
        const Case cases[] = {
            {"small", false, 0, 2000, "tuaans "},
            {"large", false, 128 * 1024, 100, "tuaans "},
            {"zen-large", true, 128 * 1024, 100, "tuaans "},
            {"zen-long", true, 512 * 1024, 20, std::string(40, 'b') + ' '},
        };
        bool matched = false;
        for (const auto &c : cases) {
            if (filter != "all" && filter != c.name) {
                continue;
            }
            matched = true;
            BenchmarkClient client(instance, c.zen, c.prefixBytes);
            client.type(engine, entry, c.keys);
            const auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < c.words; ++i) {
                client.type(engine, entry, c.keys);
            }
            const auto duration = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
            std::cout << c.name << " keys=" << c.words * c.keys.size()
                      << " ms=" << std::fixed << std::setprecision(3)
                      << duration << " peak_rss_kib=" << peakResidentKiB()
                      << std::endl;
        }
        if (!matched) {
            throw std::runtime_error("Unknown benchmark case");
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
