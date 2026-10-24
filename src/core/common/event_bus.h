#pragma once
// 进程内事件总线（核心层唯一跨模块通信方式，避免模块间直接持有引用）。
#include "core/common/types.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <typeindex>
#include <vector>

namespace lb {

class EventBus {
public:
    template <typename E>
    Subscription subscribe(std::function<void(const E&)> cb) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto key = std::type_index(typeid(E));
        const int id = ++next_id_;
        handlers_[key].push_back(Handler{id, [cb](const void* p) { cb(*static_cast<const E*>(p)); }});
        return Subscription([this, key, id] { unsubscribe(key, id); });
    }

    template <typename E>
    void publish(const E& ev) const {
        std::vector<std::function<void(const void*)>> copy;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const auto it = handlers_.find(std::type_index(typeid(E)));
            if (it == handlers_.end()) return;
            copy.reserve(it->second.size());
            for (const auto& h : it->second) copy.push_back(h.fn);
        }
        for (auto& f : copy) f(&ev);
    }

private:
    struct Handler {
        int id;
        std::function<void(const void*)> fn;
    };

    void unsubscribe(std::type_index key, int id) {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = handlers_.find(key);
        if (it == handlers_.end()) return;
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [id](const Handler& h) { return h.id == id; }), v.end());
    }

    mutable std::mutex mu_;
    int next_id_ = 0;
    std::map<std::type_index, std::vector<Handler>> handlers_;
};

} // namespace lb
