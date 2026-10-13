/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FCITX5_BAMBOO_TEST_INPUTCONTEXTFIXTURE_H
#define FCITX5_BAMBOO_TEST_INPUTCONTEXTFIXTURE_H

#include "bamboo.h"
#include <algorithm>
#include <deque>
#include <fcitx-utils/event.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

namespace bamboo_test {
using namespace fcitx;

inline void check(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// A document client, with real InputContext commit/delete/forward callbacks.
class Document final : public InputContext {
public:
    Document(Instance &instance, bool surrounding, bool hardwareKeys = false,
             bool delayedKeys = false, std::string frontendName = "ibus",
             std::string program = {})
        : InputContext(instance.inputContextManager(), program),
          instance_(instance), supportsSurrounding_(surrounding),
          hardwareKeys_(hardwareKeys), delayedKeys_(delayedKeys),
          frontendName_(std::move(frontendName)) {
        created();
        setCapabilityFlags(CapabilityFlags(CapabilityFlag::Preedit) |
                           CapabilityFlag::FormattedPreedit |
                           (surrounding ? CapabilityFlag::SurroundingText
                                        : CapabilityFlag::NoFlag));
        focusIn();
        publish();
    }
    ~Document() override { destroy(); }
    const char *frontend() const override {
        return delayedKeys_ ? frontendName_.c_str() : "test-document";
    }

    void publish() {
        if (supportsSurrounding_) {
            surroundingText().setText(text, cursor, anchor);
        }
    }

    void send(BambooEngine &engine, const InputMethodEntry &entry,
              const std::string &key, bool release = false) {
        const auto previousText = text;
        const auto previousCursor = cursor;
        const auto previousAnchor = anchor;
        KeyEvent event(this, Key(key), release);
        engine.keyEvent(entry, event);
        if (!event.accepted() && !release) {
            if (key == "BackSpace") {
                backspace();
            } else if (key == "Return") {
                commitStringImpl("\n");
            } else if (key == "Tab") {
                commitStringImpl("\t");
            }
        }
        if (echoSurrounding) {
            if (staleSurrounding) {
                surroundingText().setText(previousText, previousCursor,
                                          previousAnchor);
            } else {
                publish();
            }
        }
    }

    void type(BambooEngine &engine, const InputMethodEntry &entry,
              const std::string &keys) {
        for (char key : keys) {
            send(engine, entry, key == ' ' ? "space" : std::string(1, key));
        }
    }

    void expect(const std::string &expected) {
        check(text == expected,
              "Expected '" + expected + "', got '" + text + "'");
        check(inputPanel().clientPreedit().empty(),
              "Unexpected client preedit");
        check(inputPanel().preedit().empty(), "Unexpected popup preedit");
        check(nonemptyPreedits == 0, "Sent a visible preedit");
    }

    std::string text;
    unsigned int cursor = 0;
    unsigned int anchor = 0;
    unsigned int deletes = 0;
    unsigned int backspaces = 0;
    unsigned int nonemptyPreedits = 0;
    bool echoSurrounding = true;
    bool staleSurrounding = false;
    std::vector<std::string> operations;

private:
    void commitStringImpl(const std::string &value) override {
        const auto begin = std::min(cursor, anchor);
        const auto end = std::max(cursor, anchor);
        const auto byteBegin = utf8::ncharByteLength(text.begin(), begin);
        const auto byteEnd = utf8::ncharByteLength(text.begin(), end);
        text.replace(byteBegin, byteEnd - byteBegin, value);
        cursor = anchor = begin + utf8::length(value);
        operations.push_back("commit:" + value);
    }
    void deleteSurroundingTextImpl(int offset, unsigned int size) override {
        check(supportsSurrounding_, "Unsupported surrounding text deletion");
        check(static_cast<int>(cursor) + offset >= 0,
              "Deletion before document");
        const unsigned int begin = cursor + offset;
        check(begin + size <= utf8::length(text), "Deletion past document");
        const auto byteBegin = utf8::ncharByteLength(text.begin(), begin);
        const auto byteEnd = utf8::ncharByteLength(text.begin(), begin + size);
        text.erase(byteBegin, byteEnd - byteBegin);
        cursor = anchor = begin;
        deletes++;
        operations.push_back("delete:" + std::to_string(size));
    }
    void backspace() {
        if (cursor != anchor) {
            commitStringImpl("");
        } else if (cursor) {
            const auto begin = utf8::ncharByteLength(text.begin(), cursor - 1);
            const auto end = utf8::ncharByteLength(text.begin(), cursor);
            text.erase(begin, end - begin);
            cursor = anchor = cursor - 1;
        }
    }
    void forwardKeyImpl(const ForwardKeyEvent &event) override {
        if (delayedKeys_) {
            // On GNOME/Wayland, commits can reach GTK before forwarded keys.
            // Deliver the keys on the next event-loop turn to reproduce it.
            forwardedKeys_.push_back(event);
            if (!forwardedKeySource_) {
                forwardedKeySource_ =
                    instance_.eventLoop().addDeferEvent([this](EventSource *) {
                        for (const auto &key : forwardedKeys_) {
                            applyForwardedKey(key);
                        }
                        forwardedKeys_.clear();
                        forwardedKeySource_.reset();
                        return true;
                    });
            }
            return;
        }
        applyForwardedKey(event);
    }
    void applyForwardedKey(const ForwardKeyEvent &event) {
        if (delayedKeys_ && !event.rawKey().check(FcitxKey_BackSpace)) {
            if (!event.isRelease() && event.rawKey().check(FcitxKey_Return)) {
                commitStringImpl("\n");
            } else if (!event.isRelease() &&
                       event.rawKey().check(FcitxKey_a, KeyState::Ctrl)) {
                anchor = 0;
            } else if (!event.isRelease() &&
                       event.rawKey().check(FcitxKey_space)) {
                commitStringImpl(" ");
            }
            operations.push_back("forward:" + event.rawKey().toString() +
                                 (event.isRelease() ? ":release" : ":press"));
            return;
        }
        check(event.rawKey().check(FcitxKey_BackSpace),
              "Unexpected forwarded key");
#ifdef __linux__
        // GNOME forwards the hardware code to Wayland clients. The keysym
        // alone cannot delete a character in Ghostty or IntelliJ.
        if (hardwareKeys_ && event.rawKey().code() != KEY_BACKSPACE + 8) {
            return;
        }
#endif
        if (!event.isRelease()) {
            backspace();
            backspaces++;
            operations.push_back("backspace");
        } else {
            operations.push_back("release");
        }
    }
    void updatePreeditImpl() override {
        if (!inputPanel().clientPreedit().empty()) {
            nonemptyPreedits++;
        }
    }
    Instance &instance_;
    bool supportsSurrounding_;
    bool hardwareKeys_;
    bool delayedKeys_;
    std::string frontendName_;
    std::deque<ForwardKeyEvent> forwardedKeys_;
    std::unique_ptr<EventSource> forwardedKeySource_;
};

inline void reset(BambooEngine &engine, const InputMethodEntry &entry,
                  Document &doc) {
    InputContextEvent event(&doc, EventType::InputContextReset);
    engine.reset(entry, event);
}

} // namespace bamboo_test
#endif
