/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "bamboo.h"
#include <algorithm>
#include <deque>
#include <fcitx-utils/event.h>
#include <fcitx-utils/testing.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputpanel.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

using namespace fcitx;

void check(bool condition, const std::string &message) {
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

void reset(BambooEngine &engine, const InputMethodEntry &entry, Document &doc) {
    InputContextEvent event(&doc, EventType::InputContextReset);
    engine.reset(entry, event);
}

int main() {
    try {
        setupTestingEnvironment(TEST_DATA_DIR, {}, {TEST_DATA_DIR});
        char arg0[] = "testdirectinput";
        char arg1[] = "--disable=all";
        char *argv[] = {arg0, arg1};
        Instance instance(2, argv);
        instance.addonManager().registerDefaultLoader(nullptr);
        instance.initialize();
        BambooEngine engine(&instance);
        const InputMethodEntry entry("bamboo", "Bamboo", "vi", "bamboo");
        RawConfig config;
        config.setValueByPath("DirectInput", "True");
        config.setValueByPath("SpellCheck", "False");
        config.setValueByPath("Macro", "False");
        engine.setConfig(config);

        for (bool surrounding : {true, false}) {
            Document doc(instance, surrounding);
            doc.type(engine, entry, "dd");
            doc.expect("đ");
            doc.type(engine, entry, "uo");
            doc.expect("đuo");
            doc.type(engine, entry, "oi");
            doc.expect("đuôi");
            doc.send(engine, entry, "space");
            doc.expect("đuôi ");
            doc.type(engine, entry, "Tieengs");
            doc.expect("đuôi Tiếng");
            doc.send(engine, entry, "space");
            doc.type(engine, entry, "Vieetj");
            doc.expect("đuôi Tiếng Việt");
            doc.send(engine, entry, "Return");
            doc.expect("đuôi Tiếng Việt\n");
            doc.type(engine, entry, "hello ");
            doc.expect("đuôi Tiếng Việt\nhello ");
            check(surrounding ? doc.deletes > 0 && doc.backspaces == 0
                              : doc.deletes == 0 && doc.backspaces > 0,
                  "Wrong replacement mechanism");

            doc.type(engine, entry, "as");
            doc.expect("đuôi Tiếng Việt\nhello á");
            doc.send(engine, entry, "BackSpace");
            doc.expect("đuôi Tiếng Việt\nhello ");
            doc.type(engine, entry, "a");
            doc.send(engine, entry, "s", true);
            doc.expect("đuôi Tiếng Việt\nhello a");
            doc.send(engine, entry, "Control+a");
            doc.expect("đuôi Tiếng Việt\nhello a");
            doc.type(engine, entry, "s");
            doc.expect("đuôi Tiếng Việt\nhello as");

            InputContextEvent switchEvent(
                &doc, EventType::InputContextInputMethodDeactivated);
            engine.deactivate(entry, switchEvent);
            doc.expect("đuôi Tiếng Việt\nhello as");
            doc.type(engine, entry, "s");
            doc.expect("đuôi Tiếng Việt\nhello ass");
            InputContextEvent focusEvent(&doc, EventType::InputContextFocusOut);
            engine.deactivate(entry, focusEvent);
            doc.expect("đuôi Tiếng Việt\nhello ass");
        }

#ifdef __linux__
        {
            Document doc(instance, false, true);
            doc.type(engine, entry, "dduooi");
            doc.expect("đuôi");
            doc.send(engine, entry, "space");
            doc.type(engine, entry, "truowngf vawn tuaans laapj trinhf vieen ");
            doc.expect("đuôi trường văn tuấn lập trình viên ");
            doc.send(engine, entry, "BackSpace");
            doc.expect("đuôi trường văn tuấn lập trình viên");
            check(doc.backspaces > 0,
                  "Wayland client never received Backspace");
        }
#endif

        {
            Document doc(instance, true);
            doc.type(engine, entry, "a");
            doc.cursor = doc.anchor = 0;
            doc.publish();
            doc.type(engine, entry, "s");
            doc.expect("sa");
            check(doc.deletes == 0, "Edited text after cursor movement");
            reset(engine, entry, doc);
            doc.cursor = doc.anchor = 2;
            doc.publish();
            doc.type(engine, entry, "a");
            doc.text = "external edit";
            doc.cursor = doc.anchor = utf8::length(doc.text);
            doc.publish();
            doc.type(engine, entry, "s");
            doc.expect("external edits");
            check(doc.deletes == 0, "Edited text changed by the application");
        }

        {
            Document doc(instance, true);
            doc.type(engine, entry, "a");
            doc.anchor = 0;
            doc.publish();
            doc.type(engine, entry, "s");
            doc.expect("s");
            doc.type(engine, entry, "a");
            doc.expect("sa");
            check(doc.deletes == 0, "Accent replacement crossed a selection");
        }

        {
            Document doc(instance, false);
            doc.type(engine, entry, "oos");
            doc.expect("ố");
            check(doc.operations ==
                      std::vector<std::string>{
                          "commit:o", "backspace", "release", "commit:ô",
                          "backspace", "release", "commit:ố"},
                  "Replacement events out of order");
            reset(engine, entry, doc);
            doc.type(engine, entry, "s");
            doc.expect("ốs");
        }

        {
            Document doc(instance, true);
            doc.echoSurrounding = false;
            // Rapid input must also work before the client echoes our changes.
            doc.type(engine, entry, "Vieetj");
            doc.expect("Việt");
            doc.send(engine, entry, "space");
            doc.type(engine, entry, "Nam");
            doc.expect("Việt Nam");
        }

        {
            Document doc(instance, true);
            doc.text = "selected";
            doc.cursor = 8;
            doc.anchor = 0;
            doc.publish();
            doc.echoSurrounding = false;
            doc.send(engine, entry, "Left");
            check(doc.surroundingText().text() == "selected" &&
                      doc.surroundingText().cursor() == 8 &&
                      doc.surroundingText().anchor() == 0,
                  "An unhandled key changed the cached selection");
            check(doc.operations.empty(),
                  "An unhandled key edited the document");
        }

        {
            Document doc(instance, true);
            doc.type(engine, entry, "a");
            const auto operations = doc.operations.size();
            KeyEvent shortcut(&doc, Key("Control+BackSpace"));
            engine.keyEvent(entry, shortcut);
            check(!shortcut.accepted(), "Swallowed Ctrl+Backspace");
            check(doc.operations.size() == operations,
                  "Changed text while handling Ctrl+Backspace");
            doc.type(engine, entry, "s");
            doc.expect("as");
        }

        {
            Document rapid(instance, false, true, true);
            rapid.type(engine, entry, "dduooi");
            rapid.send(engine, entry, "Return");
            rapid.send(engine, entry, "Return", true);
            rapid.type(engine, entry, "Vieetj Nam");

            Document deletion(instance, false, true, true);
            deletion.type(engine, entry, "aas");
            deletion.send(engine, entry, "BackSpace");
            deletion.type(engine, entry, "dd");

            Document telegram(instance, false, true, true, "dbus");
            telegram.type(engine, entry,
                          "Tooi ddang tesst booj gox mowis nhes cacs banj owi, "
                          "truowngf vawn tuaans laf laapj trinhf vieen");

            // Zen's native Wayland web fields echo the previous document
            // state, even when typing slowly. That must not reset every letter.
            Document zen(instance, true, true, true, "ibus", "zen.desktop");
            zen.staleSurrounding = true;
            zen.type(engine, entry, "tuaans dduooi Vieetj Nam");

            // A fast web-field update can lag behind several accepted keys.
            Document zenDelayed(instance, true, true, true, "ibus",
                                "zen.desktop");
            zenDelayed.echoSurrounding = false;
            zenDelayed.type(engine, entry, "tua");
            zenDelayed.surroundingText().setText("", 0, 0);
            zenDelayed.type(engine, entry, "ans");

            // The same native Wayland context has reliable surrounding text
            // in Zen's address bar. It should replace vowels immediately.
            Document zenUrl(instance, true, true, true, "ibus", "zen.desktop");
            zenUrl.type(engine, entry, "Tooi");
            zenUrl.expect("Tôi");
            check(zenUrl.deletes > 0 && zenUrl.backspaces == 0,
                  "Used forwarded Backspaces in Zen's reliable address bar");
            const auto urlDeletes = zenUrl.deletes;
            zenUrl.cursor = zenUrl.anchor = 0;
            zenUrl.publish();
            zenUrl.type(engine, entry, "s");
            zenUrl.expect("sTôi");
            check(zenUrl.deletes == urlDeletes,
                  "Mistook a real cursor move in Zen for a stale update");

            // Zen using the Fcitx GTK module has reliable surrounding text.
            Document zenGtk(instance, true, true, true, "dbus", "zen.desktop");
            zenGtk.type(engine, entry, "tuaans");
            zenGtk.expect("tuấn");
            check(zenGtk.deletes > 0 && zenGtk.backspaces == 0,
                  "Disabled surrounding text for Zen's native Fcitx module");

            Document shortcut(instance, false, true, true);
            shortcut.type(engine, entry, "as");
            shortcut.send(engine, entry, "Control+a");
            shortcut.type(engine, entry, "x");

            Document resetDuringReplacement(instance, false, true, true);
            resetDuringReplacement.type(engine, entry, "as");
            reset(engine, entry, resetDuringReplacement);
            resetDuringReplacement.type(engine, entry, "x");

            {
                // Destroying a client also cancels its pending timer and keys.
                Document destroyed(instance, false, true, true);
                destroyed.type(engine, entry, "dduooi");
            }

            auto stop = instance.eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 500000, 1000,
                [&instance](EventSourceTime *, uint64_t) {
                    instance.eventLoop().exit();
                    return true;
                });
            instance.eventLoop().exec();
            rapid.expect("đuôi\nViệt Nam");
            deletion.expect("đ");
            telegram.expect("Tôi đang test bộ gõ mới nhé các bạn ơi, "
                            "trường văn tuấn là lập trình viên");
            zen.expect("tuấn đuôi Việt Nam");
            zenDelayed.expect("tuấn");
            check(zen.deletes == 0 && zen.backspaces > 0,
                  "Used stale surrounding text in Zen's Wayland bridge");
            // Moving from a web field to the address bar must retry the
            // reliable text replacement route, even with the same context.
            reset(engine, entry, zen);
            zen.staleSurrounding = false;
            zen.text.clear();
            zen.cursor = zen.anchor = 0;
            zen.publish();
            const auto forwarded = zen.backspaces;
            zen.type(engine, entry, "Tooi");
            zen.expect("Tôi");
            check(zen.deletes > 0 && zen.backspaces == forwarded,
                  "Kept the web-field fallback after a focus reset");
            shortcut.expect("x");
            resetDuringReplacement.expect("áx");
            check(std::find(rapid.operations.begin(), rapid.operations.end(),
                            "forward:Return:release") != rapid.operations.end(),
                  "Lost a queued native key release");
        }

        config.setValueByPath("Macro", "True");
        engine.setConfig(config);
        RawConfig macros;
        macros.setValueByPath("Macro/0/Key", "btw");
        macros.setValueByPath("Macro/0/Value", "by the way");
        engine.setSubConfig("macro/Telex", macros);
        {
            Document doc(instance, true);
            doc.type(engine, entry, "btw");
            doc.expect("btw");
            doc.send(engine, entry, "space");
            doc.expect("by the way ");
            check(doc.deletes > 0,
                  "Macro expansion did not replace typed text");
        }

        // Decomposed and legacy output continue to use composition mode.
        config.setValueByPath("Macro", "False");
        config.setValueByPath("OutputCharset", "Unicode tổ hợp");
        engine.setConfig(config);
        {
            Document doc(instance, true);
            doc.type(engine, entry, "as");
            check(doc.text.empty(), "Decomposed output used direct input");
            check(!doc.inputPanel().clientPreedit().empty(),
                  "Decomposed output lost its composition");
        }

        // Turning direct input off retains the existing composition behavior.
        config.setValueByPath("DirectInput", "False");
        config.setValueByPath("OutputCharset", "Unicode");
        engine.setConfig(config);
        {
            Document doc(instance, true);
            doc.type(engine, entry, "as");
            check(doc.text.empty(), "Composition mode committed too early");
            check(doc.inputPanel().clientPreedit().toString() == "á",
                  "Composition mode lost its preedit");
            doc.send(engine, entry, "space");
            check(doc.text == "á ", "Composition commit changed");
        }
        std::cout << "Direct input tests passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
