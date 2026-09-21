#pragma once

#include <QString>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <utility>

namespace dvs::ui {

// Small byte-budgeted LRU cache used by the still-image loader and the derived-image
// cache. Access is single-threaded: callers perform lookup/insert on the owning QObject's
// thread only, while worker threads merely carry values back to it.
template <typename T> class ByteLruCache final {
public:
    using CostFunction = std::function<qint64(const T&)>;

    explicit ByteLruCache(const qint64 maximumBytes, CostFunction cost)
        : maximumBytes_(maximumBytes < 0 ? 0 : maximumBytes), cost_(std::move(cost)) {}

    [[nodiscard]] bool get(const QString& key, T* value) {
        const auto iterator = entries_.find(key);
        if (iterator == entries_.end()) {
            ++misses_;
            return false;
        }
        order_.splice(order_.begin(), order_, iterator->second.order);
        if (value != nullptr) {
            *value = iterator->second.value;
        }
        ++hits_;
        return true;
    }

    [[nodiscard]] bool contains(const QString& key) const {
        return entries_.find(key) != entries_.end();
    }

    void put(const QString& key, T value) {
        const qint64 cost = cost_ ? std::max<qint64>(0, cost_(value)) : 0;
        const auto existing = entries_.find(key);
        if (existing != entries_.end()) {
            currentBytes_ -= existing->second.cost;
            order_.erase(existing->second.order);
            entries_.erase(existing);
        }
        if (cost > maximumBytes_) {
            trim();
            return;
        }
        order_.push_front(key);
        entries_.emplace(key, Entry{std::move(value), cost, order_.begin()});
        currentBytes_ += cost;
        trim();
    }

    void remove(const QString& key) {
        const auto iterator = entries_.find(key);
        if (iterator == entries_.end()) {
            return;
        }
        currentBytes_ -= iterator->second.cost;
        order_.erase(iterator->second.order);
        entries_.erase(iterator);
    }

    void clear() {
        entries_.clear();
        order_.clear();
        currentBytes_ = 0;
    }

    void setMaximumBytes(const qint64 maximumBytes) {
        maximumBytes_ = maximumBytes < 0 ? 0 : maximumBytes;
        trim();
    }

    [[nodiscard]] qint64 maximumBytes() const noexcept {
        return maximumBytes_;
    }

    [[nodiscard]] qint64 currentBytes() const noexcept {
        return currentBytes_;
    }

    [[nodiscard]] int size() const noexcept {
        return static_cast<int>(entries_.size());
    }

    [[nodiscard]] quint64 hits() const noexcept {
        return hits_;
    }

    [[nodiscard]] quint64 misses() const noexcept {
        return misses_;
    }

    void resetStats() noexcept {
        hits_ = 0;
        misses_ = 0;
    }

private:
    struct Entry final {
        T value;
        qint64 cost = 0;
        std::list<QString>::iterator order;
    };

    void trim() {
        while (currentBytes_ > maximumBytes_ && !order_.empty()) {
            const QString key = order_.back();
            order_.pop_back();
            const auto iterator = entries_.find(key);
            if (iterator == entries_.end()) {
                continue;
            }
            currentBytes_ -= iterator->second.cost;
            entries_.erase(iterator);
        }
    }

    qint64 maximumBytes_ = 0;
    qint64 currentBytes_ = 0;
    quint64 hits_ = 0;
    quint64 misses_ = 0;
    CostFunction cost_;
    std::list<QString> order_;
    std::map<QString, Entry> entries_;
};

} // namespace dvs::ui
