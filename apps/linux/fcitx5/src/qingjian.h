#pragma once
// 青简输入法 fcitx5 引擎：把 fcitx5 的按键转发给 qingjian-server（Rust，独立进程），
// 按回复决定吃掉 / 放行，并把 server 的组句状态画到 fcitx5 的输入面板（preedit + 候选）。
//
// 配置：引擎通过 FCITX_CONFIGURATION 暴露官方配置（general / shortcut / fuzzy / dictionaries / model），
// fcitx5-configtool 里可编辑；保存时把可迁移的键写成 `~/.config/qingjian/config.fcitx5.toml`，
// server 启动与热加载时按「fcitx5 侧优先」合并进 Config（见 server 的 dispatch::merge_config）。
//
// 注意：FCITX_CONFIGURATION 是变参宏，实参（按顶层逗号拆分）超过 127 个会被编译器截断，
// 所以配置拆成两层（QingjianFuzzyConfig 作基类），每个宏的参数都远低于上限。

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <fcitx/addonfactory.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>
#include <fcitx-config/configuration.h>
#include <fcitx-config/enum.h>

#include "ipc.h"

namespace fcitx {
class InputContext;
}

// —— 候选窗口外观（[general] theme）——
FCITX_CONFIG_ENUM(ThemeMode, System, Light, Dark)

// —— 候选排布（[general] layout）——
FCITX_CONFIG_ENUM(LayoutMode, Vertical, Horizontal)

// —— 组句拼音显示位置（[general] preedit）——
FCITX_CONFIG_ENUM(PreeditMode, Both, Inline, Window)

// —— 青简配置（键名与官方 config.toml 字段一致）——
// fcitx5 的 FCITX_CONFIGURATION 变参宏在 5.1.21 实测约 13 个选项（~55 实参）后
// 展开会被截断，因此拆成两个配置类：QingjianExtConfig（基类，shortcut 剩余 +
// 模糊音/词库/模型）+ QingjianConfig（继承，general + 常用 shortcut）。
// —— 青简配置（键名与官方 config.toml 字段一致）——
// 手动定义配置类（fcitx::Configuration + fcitx::Option 成员自动注册），
// 不使用 FCITX_CONFIGURATION 变参宏（5.1.21 下嵌套展开不稳定）。
class QingjianConfig : public ::fcitx::Configuration {
public:
    const char *typeName() const override { return "QingjianConfig"; }

    // [general]
    ::fcitx::Option<int, ::fcitx::IntConstrain> pageSize{this, "page_size", "每页候选数", 9,
                                       ::fcitx::IntConstrain(1, 9)};
    ::fcitx::Option<std::string> pageKeys{this, "page_keys", "翻页键对", "[]"};
    ::fcitx::Option<ThemeMode> theme{this, "theme", "候选窗口外观", ThemeMode::System};
    ::fcitx::Option<LayoutMode> layout{this, "layout", "候选排布", LayoutMode::Vertical};
    ::fcitx::Option<PreeditMode> preedit{this, "preedit", "组句拼音显示", PreeditMode::Both};
    ::fcitx::Option<bool> englishCandidates{this, "english_candidates", "英文候选", true};
    ::fcitx::Option<bool> fullWidthPunctuation{this, "full_width_punctuation",
                                      "中文模式全角标点", true};
    ::fcitx::Option<std::string> shuangpin{this, "shuangpin", "双拼方案", ""};
    ::fcitx::Option<std::string> learningLanguage{this, "learning_language", "学习语言", "en"};

    // [shortcut]
    ::fcitx::Option<std::string> expression{this, "expression", "表达式模式键", "v"};
    ::fcitx::Option<std::string> question{this, "question", "问字模式键", "u"};
    ::fcitx::Option<std::string> translation{this, "translation", "上屏第一个译词", "option"};
    ::fcitx::Option<std::string> translationSecond{this, "translation_second",
                                          "上屏第二个译词", "shift+option"};
    ::fcitx::Option<std::string> translateSelection{this, "translate_selection",
                                           "翻译选中文字", "control+option+t"};
    ::fcitx::Option<std::string> deleteCandidate{this, "delete_candidate", "删除候选", "shift"};

    // [fuzzy]（合并为逗号分隔项）+ [dictionaries] + [model]
    ::fcitx::Option<std::string> fuzzyRules{this, "fuzzy_rules", "模糊音", ""};
    // 随包领域词库：勾选式开关（与 Windows「词库」页一致）；默认只开成语，对齐 server DEFAULT_DOMAINS。
    // key = 词库文件名主干（config.toml [dictionaries] domains 认的值）。
    ::fcitx::Option<bool> dictAnimals{this, "dict_animals", "动物词库", false};
    ::fcitx::Option<bool> dictAutomotive{this, "dict_automotive", "汽车词库", false};
    ::fcitx::Option<bool> dictFinance{this, "dict_finance", "财经词库", false};
    ::fcitx::Option<bool> dictFood{this, "dict_food", "饮食词库", false};
    ::fcitx::Option<bool> dictHistoricalFigures{this, "dict_historical_figures",
                                       "历史人物词库", false};
    ::fcitx::Option<bool> dictIdioms{this, "dict_idioms", "成语词库", true};
    ::fcitx::Option<bool> dictItComputing{this, "dict_it_computing", "IT 与计算机词库", false};
    ::fcitx::Option<bool> dictLaw{this, "dict_law", "法律词库", false};
    ::fcitx::Option<bool> dictMedicine{this, "dict_medicine", "医学词库", false};
    ::fcitx::Option<bool> dictPlaces{this, "dict_places", "地名词库", false};
    ::fcitx::Option<bool> dictPoetryLines{this, "dict_poetry_lines", "诗词名句词库", false};
    ::fcitx::Option<bool> modelEnabled{this, "model_enabled", "本地整句模型", true};
};
class QingjianEngine : public fcitx::InputMethodEngine {
public:
    QingjianEngine(fcitx::Instance *instance);
    ~QingjianEngine() override;

    // 名称由 qingjian.conf 的 [Addon] Name 提供，无需在引擎上实现 name()。
    // 输入法条目必须在这里提供：fcitx5 靠 listInputMethods() 收集 addon 的输入法，
    // 不实现（默认返回空）就会出现 “Found 0 input method(s) in addon”。
    std::vector<fcitx::InputMethodEntry> listInputMethods() override;
    void keyEvent(const fcitx::InputMethodEntry &entry,
                  fcitx::KeyEvent &keyEvent) override;
    void activate(const fcitx::InputMethodEntry &entry,
                  fcitx::InputContextEvent &event) override;
    void deactivate(const fcitx::InputMethodEntry &entry,
                    fcitx::InputContextEvent &event) override;
    void reset(const fcitx::InputMethodEntry &entry,
               fcitx::InputContextEvent &event) override;

    // 配置界面：fcitx5-configtool 里打开「青简」即可编辑；保存时把可迁移键写进
    // config.fcitx5.toml 供 server 合并（见 saveQingjianConfig）。
    const fcitx::Configuration *getConfig() const override { return &config_; }
    void setConfig(const fcitx::RawConfig &config) override;
    void reloadConfig() override;

private:
    // 取得 / 新建会话；连接失败返回 0（消息发不出去时用）。
    uint64_t openSession(fcitx::InputContext *ic);
    void closeSession(fcitx::InputContext *ic);

    // 失焦上屏：发 Commit 拿回缓冲并提交；不关会话（保活复用）。
    void commitSession(fcitx::InputContext *ic);

    // 处理一次 Key 的回复：上屏 / 更新 preedit 与候选；返回 true 表示按键被吃掉。
    bool handleKeyResult(fcitx::InputContext *ic, const qj::Value &reply);
    void applyFrame(fcitx::InputContext *ic, const qj::Value &frame);
    // 候选列表显示在 fcitx5 面板；空帧收掉。
    void clearPanel(fcitx::InputContext *ic);

    // 把当前配置序列化成 TOML，写入 ~/.config/qingjian/config.fcitx5.toml。
    void saveQingjianConfig();

    fcitx::Instance *instance_;
    QingjianConfig config_;
    // InputContext 销毁时清会话（防 session 泄漏与指针复用误用）。
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> icDestroyedHandler_;
    std::unique_ptr<IPCClient> ipc_;
    uint64_t nextSession_ = 1;
    std::unordered_map<fcitx::InputContext *, uint64_t> sessions_;
};
