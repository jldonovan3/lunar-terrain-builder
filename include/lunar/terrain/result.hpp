#pragma once

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <lunar/terrain/error.hpp>

namespace lunar::terrain {

template <typename T>
class [[nodiscard]] Result {
    static_assert(!std::is_reference_v<T>);

public:
    [[nodiscard]] static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    [[nodiscard]] static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept { return value_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & noexcept {
        auto* item = std::get_if<0>(&value_);
        assert(item != nullptr);
        return *item;
    }

    [[nodiscard]] const T& value() const& noexcept {
        const auto* item = std::get_if<0>(&value_);
        assert(item != nullptr);
        return *item;
    }

    [[nodiscard]] T&& value() && noexcept {
        auto* item = std::get_if<0>(&value_);
        assert(item != nullptr);
        return std::move(*item);
    }

    [[nodiscard]] Error& error() & noexcept {
        auto* item = std::get_if<1>(&value_);
        assert(item != nullptr);
        return *item;
    }

    [[nodiscard]] const Error& error() const& noexcept {
        const auto* item = std::get_if<1>(&value_);
        assert(item != nullptr);
        return *item;
    }

    [[nodiscard]] Error&& error() && noexcept {
        auto* item = std::get_if<1>(&value_);
        assert(item != nullptr);
        return std::move(*item);
    }

private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> index, Value&& value)
        : value_(index, std::forward<Value>(value)) {}

    std::variant<T, Error> value_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    [[nodiscard]] static Result success() noexcept { return Result(); }
    [[nodiscard]] static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    void value() const noexcept { assert(has_value()); }

    [[nodiscard]] Error& error() & noexcept {
        assert(error_.has_value());
        return *error_;
    }

    [[nodiscard]] const Error& error() const& noexcept {
        assert(error_.has_value());
        return *error_;
    }

private:
    Result() = default;
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

}  // namespace lunar::terrain

