#pragma once

#ifndef KAELUM_EXPECTED_HPP
#define KAELUM_EXPECTED_HPP

#include <variant>
#include <utility>
#include <type_traits>
#include <exception>
#include <string>

namespace Kaelum {

template<typename T, typename E>
class Expected {
public:
    Expected() = default;
    
    Expected(const T& value) : data_(std::in_place_index<0>, value) {}
    Expected(T&& value) : data_(std::in_place_index<0>, std::move(value)) {}
    
    Expected(const E& error) : data_(std::in_place_index<1>, error) {}
    Expected(E&& error) : data_(std::in_place_index<1>, std::move(error)) {}
    
    Expected(const Expected&) = default;
    Expected(Expected&&) = default;
    Expected& operator=(const Expected&) = default;
    Expected& operator=(Expected&&) = default;
    
    ~Expected() = default;
    
    explicit operator bool() const noexcept {
        return data_.index() == 0;
    }
    
    bool has_value() const noexcept {
        return data_.index() == 0;
    }
    
    T& value() & {
        if (data_.index() != 0) throw std::bad_variant_access();
        return std::get<0>(data_);
    }
    
    const T& value() const& {
        if (data_.index() != 0) throw std::bad_variant_access();
        return std::get<0>(data_);
    }
    
    T&& value() && {
        if (data_.index() != 0) throw std::bad_variant_access();
        return std::get<0>(std::move(data_));
    }
    
    E& error() & {
        if (data_.index() != 1) throw std::bad_variant_access();
        return std::get<1>(data_);
    }
    
    const E& error() const& {
        if (data_.index() != 1) throw std::bad_variant_access();
        return std::get<1>(data_);
    }
    
    E&& error() && {
        if (data_.index() != 1) throw std::bad_variant_access();
        return std::get<1>(std::move(data_));
    }
    
    template<typename U = T>
    U value_or(U&& default_value) & {
        if (has_value()) return value();
        return std::forward<U>(default_value);
    }
    
    template<typename U = T>
    U value_or(U&& default_value) && {
        if (has_value()) return std::move(value());
        return std::forward<U>(default_value);
    }
    
    void reset() {
        data_.reset();
    }

private:
    std::variant<T, E> data_;
};

template<typename E>
class Expected<void, E> {
public:
    Expected() : data_(std::in_place_index<0>) {}
    
    Expected(const E& error) : data_(std::in_place_index<1>, error) {}
    Expected(E&& error) : data_(std::in_place_index<1>, std::move(error)) {}
    
    Expected(const Expected&) = default;
    Expected(Expected&&) = default;
    Expected& operator=(const Expected&) = default;
    Expected& operator=(Expected&&) = default;
    
    ~Expected() = default;
    
    explicit operator bool() const noexcept {
        return data_.index() == 0;
    }
    
    bool has_value() const noexcept {
        return data_.index() == 0;
    }
    
    void value() const {
        if (data_.index() != 0) throw std::bad_variant_access();
    }
    
    E& error() & {
        if (data_.index() != 1) throw std::bad_variant_access();
        return std::get<1>(data_);
    }
    
    const E& error() const& {
        if (data_.index() != 1) throw std::bad_variant_access();
        return std::get<1>(data_);
    }
    
    E&& error() && {
        if (data_.index() != 1) throw std::bad_variant_access();
        return std::get<1>(std::move(data_));
    }
    
    void reset() {
        data_.reset();
    }

private:
    std::variant<std::monostate, E> data_;
};

template<typename E>
auto make_unexpected(E&& error) {
    return Expected<void, std::decay_t<E>>(std::forward<E>(error));
}

inline auto make_unexpected(const char* error) {
    return Expected<void, std::string>(std::string(error));
}

} // namespace Kaelum

namespace Kaelum {
    template<typename T, typename E>
    using ExpectedT = Expected<T, E>;
}

#endif