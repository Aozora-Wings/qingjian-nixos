#include "qingjian.h"

#include <algorithm>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fcitx-utils/standardpath.h>
#include <fcitx-config/iniparser.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>
#include <fcitx/event.h>
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
    // 预热：fcitx5 加载 addon 时就连好 server（不发消息），首键免 connect 握手。
    // server 未起时静默失败，后续按键会按需重连。
    ipc_->warmup();

    // ic 销毁时清理会话：deactivate 保活后，session 的清理从"失焦"挪到"窗口关闭"，
    // 否则指针复用会把旧 session 误发给新窗口。
    icDestroyedHandler_ = instance_->watchEvent(
        fcitx::EventType::InputContextDestroyed,
        fcitx::EventWatcherPhase::PreInputMethod,
        [this](fcitx::Event &event) {
            auto &icEvent = static_cast<fcitx::InputContextEvent &>(event);
            closeSession(icEvent.inputContext());
        });
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

    // ic 销毁（窗口关闭）：先 Commit 拿回缓冲（此时无焦点，上屏多是无害的），
    // 再 CloseSession 释放 server 端会话。
    commitSession(ic);

    qj::Value close = qj::Value::makeObject();
    qj::Value closeBody = qj::Value::makeObject();
    closeBody.set("session", qj::Value::makeNumber(session));
    close.set("CloseSession", std::move(closeBody));
    ipc_->request(close);
}

void QingjianEngine::commitSession(fcitx::InputContext *ic) {
    auto found = sessions_.find(ic);
    if (found == sessions_.end()) return;
    uint64_t session = found->second;

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
}

void QingjianEngine::activate(const fcitx::InputMethodEntry & /*entry*/,
                              fcitx::InputContextEvent &event) {
    // 不做 IPC：session 在首键 keyEvent 里懒建（openSession）。
    // 窗口切换时 fcitx5 调 activate/deactivate，这里任何同步请求都会拖主线程。
}

void QingjianEngine::deactivate(const fcitx::InputMethodEntry & /*entry*/,
                                fcitx::InputContextEvent &event) {
    fcitx::InputContext *ic = event.inputContext();
    if (!ic) return;
    // 只 Commit 上屏（1 次 IPC），保活 session：同一窗口切回时零 IPC。
    // session 的 CloseSession 延到 ic 销毁（窗口关闭）时做。
    commitSession(ic);
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
        int pageIndex = 0;  // 页内序号（0-based），显示 1-based，与 server 数字选词 digit-1 对齐
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
            // 页内序号前缀（1-9）：DisplayOnlyCandidateList 不画序号，UI 只显示文本；
            // server 数字选词 = page*page_size + digit - 1，页内 1-based 完全对齐。
            candidateText = std::to_string(pageIndex + 1) + ". " + candidateText;
            contents.emplace_back(candidateText);
            ++pageIndex;
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


// —— 配置：fcitx5 界面可编辑；保存时把可迁移键写成 config.fcitx5.toml 供 server 合并 ——

namespace {

// ~/.config/qingjian（XDG_CONFIG_HOME 优先）；取不到家目录返回空串。
std::string qingjianConfigDir() {
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/qingjian";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/qingjian";
    }
    return {};
}

// 值都是简单 ASCII 或拼音串，只转义基本符号即可。
std::string qjTomlEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string themeToToml(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::System:
        return "system";
    case ThemeMode::Light:
        return "light";
    default:
        return "dark";
    }
}

std::string layoutToToml(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::Vertical:
        return "vertical";
    default:
        return "horizontal";
    }
}

std::string preeditToToml(PreeditMode mode) {
    switch (mode) {
    case PreeditMode::Both:
        return "both";
    case PreeditMode::Inline:
        return "inline";
    default:
        return "window";
    }
}

std::string boolToToml(bool value) { return value ? "true" : "false"; }

} // namespace

void QingjianEngine::setConfig(const fcitx::RawConfig &config) {
    config_.load(config);
    saveQingjianConfig();
}

void QingjianEngine::reloadConfig() {
    // 从 fcitx5 的 addon 配置存储读（conf/qingjian.conf）。
    fcitx::RawConfig raw;
    auto file = fcitx::StandardPath::global().open(
        fcitx::StandardPath::Type::Config, "conf/qingjian.conf",
        O_RDONLY);
    if (file.fd() >= 0) {
        fcitx::readFromIni(raw, file.fd());
    }
    config_.load(raw);
}

void QingjianEngine::saveQingjianConfig() {
    std::string dir = qingjianConfigDir();
    if (dir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return;
    }
    std::string path = dir + "/config.fcitx5.toml";
    std::string toml;
    toml += "[general]\n";
    toml += "learning_language = \"" + qjTomlEscape(*config_.learningLanguage) + "\"\n";
    toml += "page_size = " + std::to_string(*config_.pageSize) + "\n";
    toml += "page_keys = \"" + qjTomlEscape(*config_.pageKeys) + "\"\n";
    toml += "theme = \"" + themeToToml(*config_.theme) + "\"\n";
    toml += "layout = \"" + layoutToToml(*config_.layout) + "\"\n";
    toml += "preedit = \"" + preeditToToml(*config_.preedit) + "\"\n";
    toml += "english_candidates = " + boolToToml(*config_.englishCandidates) + "\n";
    toml += "full_width_punctuation = " + boolToToml(*config_.fullWidthPunctuation) + "\n";
    toml += "shuangpin = \"" + qjTomlEscape(*config_.shuangpin) + "\"\n";
    toml += "\n[shortcut]\n";
    toml += "expression = \"" + qjTomlEscape(*config_.expression) + "\"\n";
    toml += "question = \"" + qjTomlEscape(*config_.question) + "\"\n";
    toml += "translation = \"" + qjTomlEscape(*config_.translation) + "\"\n";
    toml += "translation_second = \"" + qjTomlEscape(*config_.translationSecond) + "\"\n";
    toml += "translate_selection = \"" + qjTomlEscape(*config_.translateSelection) + "\"\n";
    toml += "delete_candidate = \"" + qjTomlEscape(*config_.deleteCandidate) + "\"\n";
    toml += "\n[fuzzy]\n";
    {
        // 模糊音：从 fuzzy_rules（逗号分隔）逐项解析，未列出的全关。
        bool zzh = false, cch = false, ssh = false, nl = false, fh = false, lr = false,
             anang = false, eneng = false, ining = false;
        const std::string rules = *config_.fuzzyRules;
        std::string token;
        for (size_t i = 0; i <= rules.size(); ++i) {
            if (i == rules.size() || rules[i] == ',' || rules[i] == ' ') {
                if (token == "z_zh") zzh = true;
                else if (token == "c_ch") cch = true;
                else if (token == "s_sh") ssh = true;
                else if (token == "n_l") nl = true;
                else if (token == "f_h") fh = true;
                else if (token == "l_r") lr = true;
                else if (token == "an_ang") anang = true;
                else if (token == "en_eng") eneng = true;
                else if (token == "in_ing") ining = true;
                token.clear();
            } else {
                token += rules[i];
            }
        }
        toml += "z_zh = " + boolToToml(zzh) + "\n";
        toml += "c_ch = " + boolToToml(cch) + "\n";
        toml += "s_sh = " + boolToToml(ssh) + "\n";
        toml += "n_l = " + boolToToml(nl) + "\n";
        toml += "f_h = " + boolToToml(fh) + "\n";
        toml += "l_r = " + boolToToml(lr) + "\n";
        toml += "an_ang = " + boolToToml(anang) + "\n";
        toml += "en_eng = " + boolToToml(eneng) + "\n";
        toml += "in_ing = " + boolToToml(ining) + "\n";
    }
    toml += "\n[dictionaries]\n";
    toml += "domains = [";
    const auto &domains = *config_.domains;
    for (size_t i = 0; i < domains.size(); ++i) {
        if (i) {
            toml += ", ";
        }
        toml += "\"" + qjTomlEscape(domains[i]) + "\"";
    }
    toml += "]\n";
    toml += "\n[model]\n";
    toml += "enabled = " + boolToToml(*config_.modelEnabled) + "\n";
    std::ofstream out(path);
    out << toml;
}

class QingjianFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new QingjianEngine(manager->instance());
    }
};

FCITX_ADDON_FACTORY(QingjianFactory);
