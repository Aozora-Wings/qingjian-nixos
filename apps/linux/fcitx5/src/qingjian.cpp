#include "qingjian.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/text.h>
#include <fcitx/candidatelist.h>
#include <fcitx/userinterface.h>
#include <fcitx/addonmanager.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysymgen.h>

namespace {

// 协议版本：与 crates/qingjian-platform/src/protocol/mod.rs 的 PROTOCOL_VERSION 对齐。
constexpr uint32_t kProtocolVersion = 4;

// 会话：fcitx5 里每个输入上下文一个。
uint64_t g_nextSession = 1;

// —— 键码映射：fcitx5 的 FcitxKey（X11 keysym）→ Windows VK 码（dispatch/key/codes.rs 的域）——

uint32_t printableToVk(int sym) {
    // 字母 a-z：keysym 0x61-0x7A → VK 0x41-0x5A。
    if (sym >= 'a' && sym <= 'z') return static_cast<uint32_t>(sym - 0x20);
    // 数字 0-9：keysym 与 VK 同值。
    if (sym >= '0' && sym <= '9') return static_cast<uint32_t>(sym);
    switch (sym) {
        case ' ': return 0x20;
        case ';': return 0xBA;
        case '=': return 0xBB;
        case ',': return 0xBC;
        case '-': return 0xBD;
        case '.': return 0xBE;
        case '/': return 0xBF;
        case '`': return 0xC0;
        case '[': return 0xDB;
        case '\\': return 0xDC;
        case ']': return 0xDD;
        case '\'': return 0xDE;
        default: return 0;
    }
}

/// keysym → (VK, character)。character 为 0 表示无字符（功能键）。
std::pair<uint32_t, int> mapKey(int sym) {
    int character = (sym >= 0x20 && sym <= 0x7E) ? sym : 0;
    switch (sym) {
        case FcitxKey_BackSpace: return {0x08, character};
        case FcitxKey_Tab: return {0x09, character};
        case FcitxKey_Return: return {0x0D, character};
        case FcitxKey_Escape: return {0x1B, character};
        case FcitxKey_Page_Up: return {0x21, character};
        case FcitxKey_Page_Down: return {0x22, character};
        case FcitxKey_End: return {0x23, character};
        case FcitxKey_Home: return {0x24, character};
        case FcitxKey_Left: return {0x25, character};
        case FcitxKey_Up: return {0x26, character};
        case FcitxKey_Right: return {0x27, character};
        case FcitxKey_Down: return {0x28, character};
        case FcitxKey_Insert: return {0x2D, character};
        case FcitxKey_Delete: return {0x2E, character};
        default: return {printableToVk(sym), character};
    }
}

/// Shift 按下的可打印字符上档：模拟 Windows ToUnicode。
int shiftedChar(int sym) {
    if (sym >= 'a' && sym <= 'z') return sym - 0x20;
    switch (sym) {
        case '`': return '~';
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '"';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        default: return sym;
    }
}

// UTF-8 字节偏移：fcitx5 Text 的光标按字节算，Frame.cursor 按 char 算。
size_t charToByte(const std::string &text, size_t chars) {
    size_t bytes = 0;
    size_t count = 0;
    while (bytes < text.size() && count < chars) {
        unsigned char c = static_cast<unsigned char>(text[bytes]);
        bytes += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        ++count;
    }
    return bytes;
}

qj::Value modifiersJson(const fcitx::KeyStates &states) {
    qj::Value modifiers = qj::Value::makeObject();
    modifiers.set("ctrl", qj::Value::makeBool(states.test(fcitx::KeyState::Ctrl)));
    modifiers.set("shift", qj::Value::makeBool(states.test(fcitx::KeyState::Shift)));
    modifiers.set("alt", qj::Value::makeBool(states.test(fcitx::KeyState::Alt)));
    modifiers.set("win", qj::Value::makeBool(states.test(fcitx::KeyState::Super)));
    // Caps Lock 亮着即英文（dispatch 里 `caps || english_mode` 决定英文）；Linux 无 Shift 单击切模式。
    modifiers.set("caps", qj::Value::makeBool(states.test(fcitx::KeyState::CapsLock)));
    modifiers.set("english_mode", qj::Value::makeBool(false));
    return modifiers;
}

qj::Value keyEventJson(uint64_t session, const fcitx::KeyEvent &keyEvent) {
    auto [vk, character] = mapKey(keyEvent.key().sym());
    bool shift = keyEvent.key().states().test(fcitx::KeyState::Shift);
    if (shift && character > 0 && character <= 0x7E) {
        character = shiftedChar(character);
    }
    qj::Value event = qj::Value::makeObject();
    event.set("virtual_key", qj::Value::makeNumber(vk));
    if (character > 0) {
        event.set("character", qj::Value::makeString(std::string(1, static_cast<char>(character))));
    } else {
        event.set("character", qj::Value());
    }
    event.set("modifiers", modifiersJson(keyEvent.key().states()));

    qj::Value message = qj::Value::makeObject();
    qj::Value key = qj::Value::makeObject();
    key.set("session", qj::Value::makeNumber(session));
    key.set("event", std::move(event));
    message.set("Key", std::move(key));
    return message;
}

}  // namespace

QingjianEngine::QingjianEngine(fcitx::Instance *instance) : instance_(instance) {
    ipc_ = std::make_unique<IPCClient>();
}

QingjianEngine::~QingjianEngine() = default;

std::vector<fcitx::InputMethodEntry> QingjianEngine::listInputMethods() {
    // InputMethodEntry(uniqueName, name, languageCode, addon)：
    // addon 参数必须等于 qingjian.conf 的 [Addon] Name，fcitx5 用它在激活时
    // 找到本引擎实例。InputMethodEntry 是 move-only，不能拷贝进 initializer_list。
    fcitx::InputMethodEntry entry("qingjian", "青简", "zh_CN", "qingjian");
    entry.setLabel("青");
    std::vector<fcitx::InputMethodEntry> list;
    list.push_back(std::move(entry));
    return list;
}

uint64_t QingjianEngine::openSession(fcitx::InputContext *ic) {
    auto found = sessions_.find(ic);
    if (found != sessions_.end()) return found->second;
    uint64_t session = g_nextSession++;
    sessions_[ic] = session;

    qj::Value message = qj::Value::makeObject();
    qj::Value open = qj::Value::makeObject();
    open.set("session", qj::Value::makeNumber(session));
    open.set("app", qj::Value());
    open.set("protocol", qj::Value::makeNumber(kProtocolVersion));
    message.set("OpenSession", std::move(open));
    ipc_->request(message);
    return session;
}

void QingjianEngine::closeSession(fcitx::InputContext *ic) {
    auto found = sessions_.find(ic);
    if (found == sessions_.end()) return;
    uint64_t session = found->second;
    sessions_.erase(found);

    // 失焦上屏：先 Commit 拿回缓冲，再关会话。
    qj::Value commit = qj::Value::makeObject();
    qj::Value body = qj::Value::makeObject();
    body.set("session", qj::Value::makeNumber(session));
    commit.set("Commit", std::move(body));
    qj::Value reply = ipc_->request(commit);
    const qj::Value *text = reply.getPath({"Committed", "text"});
    if (text && text->isString() && !text->str.empty()) {
        ic->commitString(text->str);
    }

    qj::Value close = qj::Value::makeObject();
    qj::Value closeBody = qj::Value::makeObject();
    closeBody.set("session", qj::Value::makeNumber(session));
    close.set("CloseSession", std::move(closeBody));
    ipc_->request(close);
}

void QingjianEngine::activate(const fcitx::InputMethodEntry & /*entry*/,
                              fcitx::InputContextEvent &event) {
    fcitx::InputContext *ic = event.inputContext();
    if (!ic) return;
    openSession(ic);
}

void QingjianEngine::deactivate(const fcitx::InputMethodEntry & /*entry*/,
                                fcitx::InputContextEvent &event) {
    fcitx::InputContext *ic = event.inputContext();
    if (!ic) return;
    closeSession(ic);
    clearPanel(ic);
}

void QingjianEngine::reset(const fcitx::InputMethodEntry & /*entry*/,
                           fcitx::InputContextEvent &event) {
    fcitx::InputContext *ic = event.inputContext();
    if (!ic) return;
    auto found = sessions_.find(ic);
    if (found != sessions_.end()) {
        qj::Value message = qj::Value::makeObject();
        qj::Value body = qj::Value::makeObject();
        body.set("session", qj::Value::makeNumber(found->second));
        message.set("HideCandidates", std::move(body));
        ipc_->request(message);
    }
    clearPanel(ic);
}

void QingjianEngine::keyEvent(const fcitx::InputMethodEntry & /*entry*/,
                              fcitx::KeyEvent &keyEvent) {
    if (keyEvent.isRelease()) return;  // 只在按下处理，与 Windows OnKeyDown 一致。
    fcitx::InputContext *ic = keyEvent.inputContext();
    if (!ic) return;

    uint64_t session = openSession(ic);
    if (session == 0) return;

    qj::Value reply = ipc_->request(keyEventJson(session, keyEvent));
    if (reply.isNull()) {
        // 连不上 server：不吞键，让 fcitx5 原样放行。
        return;
    }
    if (handleKeyResult(ic, reply)) {
        keyEvent.filterAndAccept();
    }
}

bool QingjianEngine::handleKeyResult(fcitx::InputContext *ic, const qj::Value &reply) {
    const qj::Value *result = reply.get("KeyResult");
    if (!result) return false;

    const qj::Value *outcome = result->get("outcome");
    std::string out = (outcome && outcome->isString()) ? outcome->str : "Passthrough";

    const qj::Value *commit = result->get("commit");
    if (commit && commit->isString() && !commit->str.empty()) {
        ic->commitString(commit->str);
    }
    const qj::Value *frame = result->get("frame");
    if (frame) applyFrame(ic, *frame);

    return out == "Consumed";
}

void QingjianEngine::applyFrame(fcitx::InputContext *ic, const qj::Value &frame) {
    const qj::Value *preedit = frame.get("preedit");
    const qj::Value *candidates = frame.get("candidates");
    const qj::Value *items = candidates ? candidates->get("items") : nullptr;
    bool empty = (!preedit || preedit->arr.empty()) && (!items || items->arr.empty());
    if (empty) {
        clearPanel(ic);
        return;
    }

    // preedit 行：拼接各段（Rest / Corrected 的样式差异 Phase 2 再做）。
    fcitx::Text preeditText;
    std::string raw;
    if (preedit) {
        for (const auto &segment : preedit->arr) {
            const qj::Value *text = segment.get("text");
            if (text && text->isString()) raw += text->str;
        }
    }
    const qj::Value *cursorValue = frame.get("cursor");
    size_t cursorChars = (cursorValue && cursorValue->isNumber()) ? static_cast<size_t>(cursorValue->num) : raw.size();
    // 整句补全（sentence）画在拼音行右侧，先拼上（Phase 2 再区分样式）。
    const qj::Value *sentence = frame.get("sentence");
    if (sentence && sentence->isString() && !sentence->str.empty()) {
        raw += " ";
        raw += sentence->str;
    }
    preeditText.append(raw);
    preeditText.setCursor(static_cast<int>(charToByte(raw, cursorChars)));
    ic->inputPanel().setPreedit(preeditText);

    // 候选页：server 已分页，插件只放当前页；翻页键由 server 吃掉后回新帧重建。
    // 用 DisplayOnlyCandidateList：它不可由 fcitx5 移动光标 / 翻页（方向键、PageUp/Down
    // 不会被 fcitx5 拦截），全部按键都到达 engine 转发给 server，高亮与分页以 server 为准。
    if (items && !items->arr.empty()) {
        std::vector<fcitx::Text> contents;
        for (const auto &item : items->arr) {
            const qj::Value *text = item.get("text");
            std::string candidateText = (text && text->isString()) ? text->str : "";
            // 译文（学习语言下第一条释义）附在候选后：词 + 空格 + 译文。
            const qj::Value *translation = item.get("translation");
            const qj::Value *senses = translation ? translation->get("senses") : nullptr;
            if (senses && !senses->arr.empty()) {
                const qj::Value *senseText = senses->arr.front().get("text");
                if (senseText && senseText->isString() && !senseText->str.empty()) {
                    candidateText += " ";
                    candidateText += senseText->str;
                }
            }
            contents.emplace_back(candidateText);
        }
        const qj::Value *highlight = frame.get("highlight");
        int highlightIndex = (highlight && highlight->isNumber()) ? static_cast<int>(highlight->num) : 0;
        auto list = std::make_unique<fcitx::DisplayOnlyCandidateList>();
        list->setContent(std::move(contents));
        list->setCursorIndex(highlightIndex);
        ic->inputPanel().setCandidateList(std::move(list));
    }

    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void QingjianEngine::clearPanel(fcitx::InputContext *ic) {
    ic->inputPanel().reset();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

class QingjianFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new QingjianEngine(manager->instance());
    }
};

FCITX_ADDON_FACTORY(QingjianFactory);
