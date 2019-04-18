#pragma once

#include <functional>
#include <type_traits>
#include <utility>

struct Handler {
	std::function<void(bool)> func;
	Handler() = default;
	template <typename F, typename = std::enable_if_t<!std::is_base_of_v<
														Handler, std::remove_reference_t<F>>>>
	Handler(F &&f) : func(std::forward<F>(f)) {}
	void Proceed(bool ok) const { func(ok); }
};
