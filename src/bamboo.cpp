/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 */

#include "bamboo.h"
#include "bambooconfig.h"
#include "surroundinghistory.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/charutils.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/macros.h>
#include <fcitx-utils/misc.h>
#include <fcitx-utils/standardpaths.h>
#include <fcitx-utils/stringutils.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/action.h>
#include <fcitx/addoninstance.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/menu.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef __linux__
#include <linux/input-event-codes.h>
#endif

namespace fcitx {

namespace {

constexpr std::string_view MacroPrefix = "macro/";
constexpr std::string_view InputMethodActionPrefix = "bamboo-input-method-";
constexpr std::string_view CharsetActionPrefix = "bamboo-charset-";
const std::string CustomKeymapFile = "conf/bamboo-custom-keymap.conf";

#ifdef __linux__
// Fcitx keys use XKB codes; GNOME converts them back to evdev codes before
// sending keyboard events to Wayland clients. A keysym with code 0 becomes
// KEY_RESERVED, so those clients cannot apply the Backspace replacement.
constexpr int BackspaceKeyCode = KEY_BACKSPACE + 8;
#else
constexpr int BackspaceKeyCode = 0;
#endif

FCITX_DEFINE_LOG_CATEGORY(bamboo, "bamboo");

std::string macroFile(std::string_view imName) {
    return stringutils::concat("conf/bamboo-macro-", imName, ".conf");
}

uintptr_t newMacroTable(const BambooMacroTable &macroTable) {
    std::vector<char *> charArray;
    RawConfig r;
    macroTable.save(r);
    for (const auto &keymap : *macroTable.macros) {
        charArray.push_back(const_cast<char *>(keymap.key->data()));
        charArray.push_back(const_cast<char *>(keymap.value->data()));
    }
    charArray.push_back(nullptr);
    return NewMacroTable(charArray.data());
}

std::vector<std::string> convertToStringList(char **array) {
    std::vector<std::string> result;
    for (int i = 0; array[i]; i++) {
        result.push_back(array[i]);
        free(array[i]);
    }
    free(array);
    return result;
}

} // namespace

#define FCITX_BAMBOO_DEBUG() FCITX_LOGC(bamboo, Debug)

class BambooState final : public InputContextProperty {
public:
    BambooState(BambooEngine *engine, InputContext *ic, EventLoop &eventLoop)
        : engine_(engine), ic_(ic), eventLoop_(eventLoop) {
        setEngine();
    }

    ~BambooState() override = default;

    void setEngine() {
        flushPendingInput();
        resetDirectTracking();
        bambooEngine_.reset();

        if (*engine_->config().inputMethod == "Custom") {
            std::vector<char *> charArray;
            for (const auto &keymap : *engine_->customKeymap().customKeymap) {
                charArray.push_back(const_cast<char *>(keymap.key->data()));
                FCITX_INFO() << charArray.back();
                charArray.push_back(const_cast<char *>(keymap.value->data()));
                FCITX_INFO() << charArray.back();
            }
            charArray.push_back(nullptr);
            bambooEngine_.reset(NewCustomEngine(charArray.data(),
                                                engine_->dictionary(),
                                                engine_->macroTable()));
        } else {
            bambooEngine_.reset(NewEngine(engine_->config().inputMethod->data(),
                                          engine_->dictionary(),
                                          engine_->macroTable()));
        }
        setOption();
    }

    void setOption() {
        if (!bambooEngine_) {
            return;
        }
        FcitxBambooEngineOption option = {
            .autoNonVnRestore = *engine_->config().autoNonVnRestore,
            .ddFreeStyle = true,
            .macroEnabled = *engine_->config().macro,
            .autoCapitalizeMacro = *engine_->config().capitalizeMacro,
            .spellCheckWithDicts = *engine_->config().spellCheck,
            .outputCharset = engine_->config().outputCharset->data(),
            .modernStyle = *engine_->config().modernStyle,
            .freeMarking = *engine_->config().freeMarking,
        };
        EngineSetOption(bambooEngine_.handle(), &option);
    }

    void keyEvent(KeyEvent &keyEvent) {
        if (waitingForCommit_) {
            // Preserve subsequent presses, releases, and navigation keys while
            // the client finishes the Backspaces for this replacement.
            pendingKeys_.push_back(keyEvent);
            keyEvent.filterAndAccept();
            return;
        }
        if (!bambooEngine_) {
            return;
        }
        // Ignore all key release.
        if (keyEvent.isRelease()) {
            return;
        }
        if (keyEvent.rawKey().check(FcitxKey_Shift_L) ||
            keyEvent.rawKey().check(FcitxKey_Shift_R)) {
            return;
        }

        std::optional<size_t> cursorByte;
        if (directInput() && !directText_.empty() &&
            !directTextMatches(cursorByte)) {
            if (isStaleZenSurrounding()) {
                // Keep the word when Zen echoes a state before our recent
                // commits. Only this field needs the native-key fallback.
                directBackspaceFallback_ = true;
                directCursor_.reset();
                directSurroundingHistory_.reset();
            } else {
                // The application moved the cursor, selected text, or edited
                // the word. Never replace text outside our current buffer.
                ResetEngine(bambooEngine_.handle());
                resetDirectTracking();
            }
        }

        if (keyEvent.key().checkKeyList(*engine_->config().restoreKeyStroke)) {
            EngineSetRestoreKeyStroke(bambooEngine_.handle());
            keyEvent.filterAndAccept();
            return;
        }

        if (directInput()) {
            const auto states = keyEvent.key().states();
            if (states.test(KeyState::Ctrl) || states.test(KeyState::Alt) ||
                states.test(KeyState::Super) || states.test(KeyState::Hyper) ||
                states.test(KeyState::Meta)) {
                // Shortcuts, including Ctrl+Backspace, belong to the editor.
                resetComposition();
                return;
            }
        }

        const auto output = EngineProcessKeyEventWithOutput(
            bambooEngine_.handle(), keyEvent.rawKey().sym(),
            keyEvent.rawKey().states());
        UniqueCPtr<char> commit(output.commit);
        UniqueCPtr<char> preedit(output.preedit);
        if (output.accepted) {
            keyEvent.filterAndAccept();
        }

        ic_->inputPanel().reset();
        if (directInput()) {
            const std::string_view committed = commit ? commit.get() : "";
            const std::string_view composing = preedit ? preedit.get() : "";
            if (committed.empty()) {
                updateDirectText(composing, cursorByte);
            } else {
                std::string text;
                text.reserve(committed.size() + composing.size());
                text.append(committed);
                text.append(composing);
                updateDirectText(text, cursorByte);
            }
            directText_.assign(composing);
            if (directText_.empty()) {
                directCursor_.reset();
                directSurroundingHistory_.clear();
            }
            ic_->updatePreedit();
            ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
            return;
        }
        if (commit && commit.get()[0]) {
            ic_->commitString(commit.get());
        }
        if (preedit && preedit.get()[0]) {
            std::string_view preeditView = preedit.get();
            Text text;
            TextFormatFlags format;
            if (ic_->capabilityFlags().test(CapabilityFlag::Preedit) &&
                *engine_->config().displayUnderline) {
                format = TextFormatFlag::Underline;
            }
            if (utf8::validate(preeditView)) {
                text.append(std::string(preeditView), format);
            }
            text.setCursor(text.textLength());

            if (ic_->capabilityFlags().test(CapabilityFlag::Preedit)) {
                ic_->inputPanel().setClientPreedit(text);
            } else {
                ic_->inputPanel().setPreedit(text);
            }
        }
        ic_->updatePreedit();
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void reset(bool immediately = false) {
        if (waitingForCommit_ && !immediately) {
            // Finish already accepted input before starting a fresh word.
            pendingKeys_.emplace_back(std::nullopt);
        } else {
            flushPendingInput();
            resetComposition();
        }
    }

    void resetComposition() {
        resetDirectTracking();
        ic_->inputPanel().reset();
        if (bambooEngine_) {
            ResetEngine(bambooEngine_.handle());
        }
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic_->updatePreedit();
    }

    void commitBuffer() {
        if (directInput()) {
            // The text is already in the document, including on focus loss.
            reset(true);
            return;
        }
        ic_->inputPanel().reset();
        if (bambooEngine_) {
            // The reason that we do not commit here is we want to force the
            // behavior. When client get unfocused, the framework will try to
            // commit the string.
            EngineCommitPreedit(bambooEngine_.handle());
            UniqueCPtr<char> commit(EnginePullCommit(bambooEngine_.handle()));
            if (commit && commit.get()[0]) {
                ic_->commitString(commit.get());
            }
        }
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
        ic_->updatePreedit();
    }

private:
    void resetDirectTracking() {
        directText_.clear();
        directCursor_.reset();
        directSurroundingHistory_.reset();
        directBackspaceFallback_ = false;
    }

    void finishPendingCommit() {
        waitingForCommit_ = false;
        auto text = std::exchange(pendingCommit_, {});
        if (!text.empty()) {
            ic_->commitString(text);
        }
        while (!waitingForCommit_ && !pendingKeys_.empty()) {
            auto pending = pendingKeys_.front();
            pendingKeys_.pop_front();
            if (!pending) {
                resetComposition();
                continue;
            }
            auto &event = *pending;
            keyEvent(event);
            if (!event.accepted()) {
                ic_->forwardKey(event.rawKey(), event.isRelease(),
                                event.time());
                if (!event.isRelease()) {
                    // A native Enter or cursor movement must reach the client
                    // before we commit text from the next queued key.
                    deferCommit({});
                }
            }
        }
    }

    void flushPendingInput() {
        while (waitingForCommit_) {
            directCommitTimer_->setEnabled(false);
            finishPendingCommit();
        }
    }

    void deferCommit(std::string text) {
        pendingCommit_ = std::move(text);
        waitingForCommit_ = true;
        // GNOME/Wayland and Qt deliver forwarded keys asynchronously.
        // Sending a commit in the same turn can insert the new vowel before
        // Backspace removes the old one (observed with Ghostty/GTK4).
        constexpr uint64_t replacementDelay = 10000;
        if (!directCommitTimer_) {
            directCommitTimer_ = eventLoop_.addTimeEvent(
                CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + replacementDelay, 1000,
                [this](EventSourceTime *, uint64_t) {
                    finishPendingCommit();
                    return true;
                });
        } else {
            directCommitTimer_->setNextInterval(replacementDelay);
        }
        directCommitTimer_->setOneShot();
    }

    bool directInput() const {
        // Backspace works on characters, not the bytes of legacy encodings or
        // the combining marks in decomposed Unicode output.
        return *engine_->config().directInput &&
               *engine_->config().outputCharset == "Unicode";
    }

    bool hasSurroundingText() const {
        return !directBackspaceFallback_ &&
               ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) &&
               ic_->surroundingText().isValid();
    }

    bool isZenWayland() const {
        return ic_->frontendName() == "ibus" && ic_->program() == "zen.desktop";
    }

    bool isStaleZenSurrounding() const {
        // Mozilla bug 2080478: native Wayland web fields may echo the state
        // before recent commits. The address bar updates correctly.
        // Match an exact old state so real edits still reset composition.
        if (!isZenWayland() || directSurroundingHistory_.empty()) {
            return false;
        }
        return directSurroundingHistory_.matches(ic_->surroundingText());
    }

    bool directTextMatches(std::optional<size_t> &cursorByte) const {
        if (!hasSurroundingText()) {
            // Clients without surrounding text notify cursor moves via reset.
            return true;
        }
        const auto &surrounding = ic_->surroundingText();
        if (surrounding.cursor() != surrounding.anchor() ||
            (directCursor_ && surrounding.cursor() != *directCursor_)) {
            return false;
        }
        const auto cursor = utf8::ncharByteLength(surrounding.text().begin(),
                                                  surrounding.cursor());
        cursorByte = cursor;
        return static_cast<size_t>(cursor) >= directText_.size() &&
               surrounding.text().compare(cursor - directText_.size(),
                                          directText_.size(), directText_) == 0;
    }

    void updateDirectText(std::string_view text,
                          std::optional<size_t> cursorByte) {
        if (text == directText_) {
            return;
        }
        // Find the common bytes, then rewind a partial UTF-8 character.
        // Vietnamese vowels often share leading bytes; never keep those alone.
        const auto commonLength = std::min(directText_.size(), text.size());
        size_t prefix = 0;
        while (prefix < commonLength && directText_[prefix] == text[prefix]) {
            ++prefix;
        }
        while (prefix && prefix < text.size() &&
               (static_cast<unsigned char>(text[prefix]) & 0xc0) == 0x80) {
            --prefix;
        }
        const auto erase =
            utf8::length(directText_, prefix, directText_.size());
        const std::string insert(text.substr(prefix));

        const bool surroundingAvailable = hasSurroundingText();
        std::string expectedText;
        unsigned int expectedCursor = 0;
        if (surroundingAvailable) {
            const auto &surrounding = ic_->surroundingText();
            expectedText = surrounding.text();
            const auto selectionStart =
                std::min(surrounding.cursor(), surrounding.anchor());
            const auto selectionEnd =
                std::max(surrounding.cursor(), surrounding.anchor());
            expectedCursor = selectionStart - erase;
            // Reuse this key's validated cursor position. The old suffix is
            // already owned UTF-8 text, so its byte length gives the deletion
            // start without rescanning the document for each endpoint.
            const auto selectionByte =
                cursorByte && selectionStart == surrounding.cursor()
                    ? *cursorByte
                    : utf8::ncharByteLength(expectedText.begin(),
                                            selectionStart);
            const auto begin = selectionByte - (directText_.size() - prefix);
            const auto end =
                selectionByte +
                utf8::ncharByteLength(expectedText.begin() + selectionByte,
                                      selectionEnd - selectionStart);
            if (isZenWayland()) {
                directSurroundingHistory_.remember(surrounding, begin - prefix,
                                                   end);
            }
            expectedText.replace(begin, end - begin, insert);
            expectedCursor += utf8::length(insert);
        }

        if (erase) {
            if (surroundingAvailable) {
                ic_->deleteSurroundingText(-static_cast<int>(erase), erase);
            } else {
                const Key backspace(FcitxKey_BackSpace, KeyStates(),
                                    BackspaceKeyCode);
                for (size_t i = 0; i < erase; i++) {
                    ic_->forwardKey(backspace);
                    ic_->forwardKey(backspace, true);
                }
            }
        }
        const auto frontend = ic_->frontendName();
        if (erase && !surroundingAvailable &&
            (frontend == "ibus" || frontend == "dbus")) {
            deferCommit(insert);
        } else if (!insert.empty()) {
            ic_->commitString(insert);
        }

        if (surroundingAvailable) {
            // Keep the cache current while the client processes our requests.
            ic_->surroundingText().setText(expectedText, expectedCursor,
                                           expectedCursor);
            directCursor_ = expectedCursor;
        } else {
            directCursor_.reset();
        }
    }

    BambooEngine *engine_;
    InputContext *ic_;
    EventLoop &eventLoop_;
    CGoObject bambooEngine_;
    std::string directText_;
    std::optional<unsigned int> directCursor_;
    SurroundingHistory directSurroundingHistory_;
    bool directBackspaceFallback_ = false;
    std::deque<std::optional<KeyEvent>> pendingKeys_;
    std::string pendingCommit_;
    bool waitingForCommit_ = false;
    std::unique_ptr<EventSourceTime> directCommitTimer_;
};

BambooEngine::~BambooEngine() = default;

BambooEngine::BambooEngine(Instance *instance)
    : instance_(instance), factory_([this](InputContext &ic) {
          return new BambooState(this, &ic, instance_->eventLoop());
      }) {
    Init();
    {
        auto imNames = convertToStringList(GetInputMethodNames());
        imNames.push_back("Custom");
        imNames_ = std::move(imNames);
    }
    if (std::find(imNames_.begin(), imNames_.end(), "Telex") ==
        imNames_.end()) {
        throw std::runtime_error("Failed to find required input method Telex");
    }
    FCITX_BAMBOO_DEBUG() << "Supported input methods: " << imNames_;
    config_.inputMethod.annotation().setList(imNames_);

    auto fd = StandardPaths::global().open(StandardPathsType::PkgData,
                                           "bamboo/vietnamese.cm.dict");
    if (!fd.isValid()) {
        throw std::runtime_error("Failed to load dictionary");
    }
    dictionary_.reset(NewDictionary(fd.release()));

    auto &uiManager = instance_->userInterfaceManager();
    inputMethodAction_ = std::make_unique<SimpleAction>();
    inputMethodAction_->setIcon("document-edit");
    inputMethodAction_->setShortText(_("Input Method"));
    uiManager.registerAction("bamboo-input-method", inputMethodAction_.get());

    inputMethodMenu_ = std::make_unique<Menu>();
    inputMethodAction_->setMenu(inputMethodMenu_.get());
    for (const auto &imName : imNames_) {
        inputMethodSubAction_.emplace_back(std::make_unique<SimpleAction>());
        auto *action = inputMethodSubAction_.back().get();
        action->setShortText(imName);
        action->setCheckable(true);
        uiManager.registerAction(
            stringutils::concat(InputMethodActionPrefix, imName), action);
        connections_.emplace_back(action->connect<SimpleAction::Activated>(
            [this, imName](InputContext *ic) {
                if (config_.inputMethod.value() == imName) {
                    return;
                }
                config_.inputMethod.setValue(imName);
                saveConfig();
                refreshEngine();
                updateInputMethodAction(ic);
            }));

        inputMethodMenu_->addAction(action);
    }

    charsetAction_ = std::make_unique<SimpleAction>();
    charsetAction_->setShortText(_("Output charset"));
    charsetAction_->setIcon("character-set");
    uiManager.registerAction("bamboo-charset", charsetAction_.get());
    charsetMenu_ = std::make_unique<Menu>();
    charsetAction_->setMenu(charsetMenu_.get());

    auto charsets = convertToStringList(GetCharsetNames());
    for (const auto &charset : charsets) {
        charsetSubAction_.emplace_back(std::make_unique<SimpleAction>());
        auto *action = charsetSubAction_.back().get();
        action->setShortText(charset);
        action->setCheckable(true);
        connections_.emplace_back(action->connect<SimpleAction::Activated>(
            [this, charset](InputContext *ic) {
                if (config_.outputCharset.value() == charset) {
                    return;
                }
                config_.outputCharset.setValue(charset);
                saveConfig();
                refreshEngine();
                updateCharsetAction(ic);
            }));
        uiManager.registerAction(
            stringutils::concat(CharsetActionPrefix, charset), action);
        charsetMenu_->addAction(action);
    }
    config_.outputCharset.annotation().setList(charsets);

    spellCheckAction_ = std::make_unique<SimpleAction>();
    spellCheckAction_->setLongText(_("Spell check"));
    spellCheckAction_->setIcon("tools-check-spelling");
    connections_.emplace_back(
        spellCheckAction_->connect<SimpleAction::Activated>(
            [this](InputContext *ic) {
                config_.spellCheck.setValue(!*config_.spellCheck);
                saveConfig();
                refreshOption();
                updateSpellAction(ic);
            }));
    uiManager.registerAction("bamboo-spell-check", spellCheckAction_.get());
    macroAction_ = std::make_unique<SimpleAction>();
    macroAction_->setLongText(_("Macro"));
    macroAction_->setIcon("edit-find");
    connections_.emplace_back(macroAction_->connect<SimpleAction::Activated>(
        [this](InputContext *ic) {
            config_.macro.setValue(!*config_.macro);
            saveConfig();
            refreshOption();
            updateMacroAction(ic);
        }));
    uiManager.registerAction("bamboo-macro", macroAction_.get());

    reloadConfig();
    instance_->inputContextManager().registerProperty("bambooState", &factory_);
}

void BambooEngine::reloadConfig() {
    readAsIni(config_, "conf/bamboo.conf");
    readAsIni(customKeymap_, CustomKeymapFile);
    for (const auto &imName : imNames_) {
        auto &table = macroTables_[imName];
        readAsIni(table, macroFile(imName));
        macroTableObject_[imName].reset(newMacroTable(table));
    }

    populateConfig();
}

const Configuration *BambooEngine::getSubConfig(const std::string &path) const {
    if (path == "custom_keymap") {
        return &customKeymap_;
    }
    if (path.starts_with(MacroPrefix)) {
        const auto imName = path.substr(MacroPrefix.size());
        if (auto iter = macroTables_.find(imName); iter != macroTables_.end()) {
            return &iter->second;
        }
        return nullptr;
    }
    return nullptr;
}

void BambooEngine::setConfig(const RawConfig &config) {
    config_.load(config, true);
    saveConfig();
    populateConfig();
}

void BambooEngine::populateConfig() {
    refreshEngine();
    refreshOption();
    updateMacroAction(nullptr);
    updateSpellAction(nullptr);
    updateInputMethodAction(nullptr);
    updateCharsetAction(nullptr);
}

void BambooEngine::setSubConfig(const std::string &path,
                                const RawConfig &config) {
    if (path == "custom_keymap") {
        customKeymap_.load(config, true);
        safeSaveAsIni(customKeymap_, CustomKeymapFile);
        refreshEngine();
    } else if (path.starts_with(MacroPrefix)) {
        const auto imName = path.substr(MacroPrefix.size());
        if (auto iter = macroTables_.find(imName); iter != macroTables_.end()) {
            iter->second.load(config, true);
            safeSaveAsIni(iter->second, macroFile(imName));
            macroTableObject_[imName].reset(newMacroTable(iter->second));
            refreshEngine();
        }
    }
}

std::string BambooEngine::subMode(const fcitx::InputMethodEntry & /*entry*/,
                                  fcitx::InputContext & /*inputContext*/) {
    return *config_.inputMethod;
}

std::string
BambooEngine::subModeLabelImpl(const fcitx::InputMethodEntry & /*entry*/,
                               fcitx::InputContext & /*inputContext*/) {
    return "vi";
}

void BambooEngine::activate(const InputMethodEntry &entry,
                            InputContextEvent &event) {
    FCITX_UNUSED(entry);
    FCITX_UNUSED(event);
    auto &statusArea = event.inputContext()->statusArea();

    updateMacroAction(event.inputContext());
    updateSpellAction(event.inputContext());
    updateInputMethodAction(event.inputContext());
    updateCharsetAction(event.inputContext());

    statusArea.addAction(StatusGroup::InputMethod, inputMethodAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, charsetAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, spellCheckAction_.get());
    statusArea.addAction(StatusGroup::InputMethod, macroAction_.get());
}

void BambooEngine::deactivate(const InputMethodEntry &entry,
                              InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    if (event.type() != EventType::InputContextFocusOut) {
        state->commitBuffer();
    } else {
        state->reset(true);
    }
}

void BambooEngine::keyEvent(const InputMethodEntry &entry, KeyEvent &keyEvent) {
    FCITX_UNUSED(entry);
    auto *state = keyEvent.inputContext()->propertyFor(&factory_);

    state->keyEvent(keyEvent);
}

void BambooEngine::reset(const InputMethodEntry &entry,
                         InputContextEvent &event) {
    FCITX_UNUSED(entry);
    auto *state = event.inputContext()->propertyFor(&factory_);
    state->reset();
}

void BambooEngine::refreshEngine() {
    FCITX_BAMBOO_DEBUG() << "Refresh engine";
    if (!factory_.registered()) {
        return;
    }

    instance_->inputContextManager().foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->setEngine();
        if (ic->hasFocus()) {
            state->reset();
        }
        return true;
    });
}

void BambooEngine::refreshOption() {
    if (!factory_.registered()) {
        return;
    }
    instance_->inputContextManager().foreach([this](InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->setOption();
        if (ic->hasFocus()) {
            state->reset();
        }
        return true;
    });
}

void BambooEngine::updateSpellAction(InputContext *ic) {
    spellCheckAction_->setChecked(*config_.spellCheck);
    spellCheckAction_->setShortText(*config_.spellCheck
                                        ? _("Spell Check Enabled")
                                        : _("Spell Check Disabled"));
    if (ic) {
        spellCheckAction_->update(ic);
    }
}

void BambooEngine::updateMacroAction(InputContext *ic) {
    macroAction_->setChecked(*config_.macro);
    macroAction_->setShortText(*config_.macro ? _("Macro Enabled")
                                              : _("Macro Disabled"));
    if (ic) {
        macroAction_->update(ic);
    }
}

void BambooEngine::updateInputMethodAction(InputContext *ic) {
    auto name =
        stringutils::concat(InputMethodActionPrefix, *config_.inputMethod);
    for (const auto &action : inputMethodSubAction_) {
        action->setChecked(action->name() == name);
        if (ic) {
            action->update(ic);
        }
    }
}

void BambooEngine::updateCharsetAction(InputContext *ic) {
    auto name =
        stringutils::concat(CharsetActionPrefix, *config_.outputCharset);
    for (const auto &action : charsetSubAction_) {
        action->setChecked(action->name() == name);
        if (ic) {
            action->update(ic);
        }
    }
}

} // namespace fcitx

FCITX_ADDON_FACTORY_V2(bamboo, fcitx::BambooFactory)
