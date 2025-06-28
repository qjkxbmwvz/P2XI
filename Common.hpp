#pragma once

#include <stdexcept>
#include <array>
#include <string>

namespace procon {
	using uchar = unsigned char;

	// RAII function object
	template<class T>
	class scoped_function {
		T t;
	public:
		explicit scoped_function(T&& f) :t(f) {}
		explicit scoped_function(const T& f) :t(f) {}
		~scoped_function() {
			t();
		}
		scoped_function& operator=(T&& f) { t = f; }
		scoped_function& operator=(const T& f) { t = f; }
	};
	template<class T>
	scoped_function<T> make_scoped(T f) {
		return scoped_function<T>(std::move(f));
	}

	enum class button {
		none,
		d_pad_up,
		d_pad_down,
		d_pad_right,
		d_pad_left,
		a,
		b,
		x,
		y,
		plus,
		minus,
		l,
		zl,
		r,
		zr,
		left_stick,
		right_stick,
		home,
		capture,
	};

	const std::array<button, 8> joycon_l_bitmap =
	{
		button::d_pad_down,
		button::d_pad_up,
		button::d_pad_right,
		button::d_pad_left,
		button::none,
		button::none,
		button::l,
		button::zl
	};

	const std::array<button, 8> joycon_r_bitmap = {
		button::y,
		button::x,
		button::b,
		button::a,
		button::none,
		button::none,
		button::r,
		button::zr
	};

	const std::array<button, 8> joycon_mid_bitmap = {
		button::minus,
		button::plus,
		button::right_stick,
		button::left_stick,
		button::home,
		button::capture,
		button::none,
		button::none
	};

	enum class button_source {
		left,
		right,
		middle
	};

	constexpr int joycon_l_id = 0x2006;
	constexpr int joycon_r_id = 0x2007;
	constexpr int procon_id = 0x2009;
	constexpr int joycon_grip_id = 0x200e;
	constexpr int nintendo_id = 0x057E;

	const std::string& button_to_string(button b); // In Controller.cpp

	unsigned char operator ""_uc(unsigned long long t); // In Controller.cpp
};
