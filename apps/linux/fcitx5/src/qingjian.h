#pragma once
// 青简输入法 fcitx5 引擎：把 fcitx5 的按键转发给 qingjian-server（Rust，独立进程），
// 按回复决定吃掉 / 放行，并把 server 的组句状态画到 fcitx5 的输入面板（preedit + 候选）。

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <fcitx/addonfactory.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/instance.h>

#include "ipc.h"

namespace fcitx {
class InputContext;
}

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

private:
    // 取得 / 新建会话；连接失败返回 0（消息发不出去时用）。
    uint64_t openSession(fcitx::InputContext *ic);
    void closeSession(fcitx::InputContext *ic);

    // 处理一次 Key 的回复：上屏 / 更新 preedit 与候选；返回 true 表示按键被吃掉。
    bool handleKeyResult(fcitx::InputContext *ic, const qj::Value &reply);
    void applyFrame(fcitx::InputContext *ic, const qj::Value &frame);
    // 候选列表显示在 fcitx5 面板；空帧收掉。
    void clearPanel(fcitx::InputContext *ic);

    fcitx::Instance *instance_;
    std::unique_ptr<IPCClient> ipc_;
    uint64_t nextSession_ = 1;
    std::unordered_map<fcitx::InputContext *, uint64_t> sessions_;
};
