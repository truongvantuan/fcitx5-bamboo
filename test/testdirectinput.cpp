/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "inputcontextfixture.h"
#include <fcitx-utils/testing.h>
#include <iostream>

using namespace fcitx;
using namespace bamboo_test;

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

        {
            // Moving a Go handle transfers ownership exactly once.
            CGoObject first(
                NewEngine("Telex", engine.dictionary(), engine.macroTable()));
            const auto handle = first.handle();
            CGoObject second(std::move(first));
            check(!first && second.handle() == handle,
                  "Move construction retained a Go handle in its source");
            CGoObject third(
                NewEngine("Telex", engine.dictionary(), engine.macroTable()));
            third = std::move(second);
            check(!second && third.handle() == handle,
                  "Move assignment retained a Go handle in its source");
            CGoObject &alias = third;
            third = std::move(alias);
            check(third.handle() == handle, "Self-move lost a Go handle");
            EngineProcessKeyEvent(third.handle(), FcitxKey_a, 0);
        }

        for (bool surrounding : {true, false}) {
            Document doc(instance, surrounding);
            doc.type(engine, entry, "aasf");
            doc.expect("ầ");
        }

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

            Document zenMiddle(instance, true, true, true, "ibus",
                               "zen.desktop");
            zenMiddle.text = "trước  sau";
            zenMiddle.cursor = zenMiddle.anchor = utf8::length("trước ");
            zenMiddle.publish();
            zenMiddle.staleSurrounding = true;
            zenMiddle.type(engine, entry, "tuaans");

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
            zenMiddle.expect("trước tuấn sau");
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
