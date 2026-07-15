#include "registration/updatable.h"

#include <algorithm>

namespace registration {

// 说明：make() 先递归更新依赖，再更新自身；
// 若 update_impl() 返回 true，触发 touch() 提升版本并通知观察者。

void Updatable::set_enabled(bool value) {
    enabled_ = value;
}

bool Updatable::enabled() const {
    return enabled_;
}

void Updatable::set_locked(bool value) {
    locked_ = value;
}

bool Updatable::locked() const {
    return locked_;
}

void Updatable::add_dependency(const std::shared_ptr<Updatable>& dep) {
    if (!dep) {
        return;
    }
    dependencies_.push_back(dep);
}

void Updatable::remove_dependency(const std::shared_ptr<Updatable>& dep) {
    if (!dep) {
        return;
    }
    dependencies_.erase(std::remove_if(dependencies_.begin(), dependencies_.end(),
                                       [&](const std::weak_ptr<Updatable>& w) {
                                           auto s = w.lock();
                                           return !s || s == dep;
                                       }),
                        dependencies_.end());
}

void Updatable::add_observer(NotifyFn observer) {
    observers_.push_back(std::move(observer));
}

bool Updatable::make() {
    if (!enabled_ || locked_) {
        return false;
    }

    for (auto it = dependencies_.begin(); it != dependencies_.end();) {
        auto dep = it->lock();
        if (!dep) {
            it = dependencies_.erase(it);
            continue;
        }
        dep->make();
        ++it;
    }

    const bool changed = update_impl();
    if (changed) {
        touch();
    }
    return changed;
}

void Updatable::force_update() {
    if (update_impl()) {
        touch();
    }
}

int Updatable::version() const {
    return version_;
}

bool Updatable::newer_than(const Updatable& other) const {
    return version_ > other.version_;
}

void Updatable::set_debug_name(std::string name) {
    debug_name_ = std::move(name);
}

const std::string& Updatable::debug_name() const {
    return debug_name_;
}

void Updatable::touch() {
    ++version_;
    for (auto& observer : observers_) {
        if (observer) {
            observer(*this);
        }
    }
}

UpdatableValue::UpdatableValue(Value value) : value_(std::move(value)) {}

bool UpdatableValue::update_impl() {
    return false;
}

}  // namespace registration


