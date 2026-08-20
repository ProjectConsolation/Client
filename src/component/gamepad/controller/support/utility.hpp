#pragma once

#include "component/gamepad/controller/support/types.hpp"

namespace gamepad::unstable
{
	template <typename Signature>
	class function_ref;

	template <typename Return, typename... Args>
	class function_ref<Return(Args...)>
	{
	public:
		template <typename Callable>
		function_ref(Callable&& callable) noexcept
			: object_(std::addressof(callable))
			, callback_([](const void* object, Args... args) -> Return
			{
				return (*static_cast<const std::remove_reference_t<Callable>*>(object))(
					std::forward<Args>(args)...);
			})
		{
		}

		Return operator()(Args... args) const
		{
			return callback_(object_, std::forward<Args>(args)...);
		}

	private:
		const void* object_;
		Return (*callback_)(const void*, Args...);
	};
}
