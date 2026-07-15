#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace registration {

// Updatable 提供轻量依赖更新框架：
// - 通过依赖链 make() 触发更新；
// - 通过 version 与 observer 通知状态变化。

class Updatable : public std::enable_shared_from_this<Updatable> {
public:
    using NotifyFn = std::function<void(Updatable&)>;

    virtual ~Updatable() = default;

    void set_enabled(bool value);
    bool enabled() const;

    void set_locked(bool value);
    bool locked() const;

    void add_dependency(const std::shared_ptr<Updatable>& dep);
    void remove_dependency(const std::shared_ptr<Updatable>& dep);

    void add_observer(NotifyFn observer);

    bool make();
    void force_update();

    int version() const;
    bool newer_than(const Updatable& other) const;

    void set_debug_name(std::string name);
    const std::string& debug_name() const;

protected:
    virtual bool update_impl() = 0;
    void touch();

private:
    bool enabled_ = true;
    bool locked_ = false;
    int version_ = 0;
    std::string debug_name_;
    std::vector<std::weak_ptr<Updatable>> dependencies_;
    std::vector<NotifyFn> observers_;
};

class UpdatableValue : public Updatable {
public:
    using Value = std::variant<std::string, int, double, bool>;

    explicit UpdatableValue(Value value = {});

    template <typename T>
    void set(T value) {
        value_ = Value(std::move(value));
        touch();
    }

    template <typename T>
    T get() const {
        return std::get<T>(value_);
    }

protected:
    bool update_impl() override;

private:
    Value value_;
};

}  // namespace registration


