//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___TYPE_TRAITS_IS_SWAPPABLE_H
#define _LIBCPP___TYPE_TRAITS_IS_SWAPPABLE_H

#include <__config>
#include <__type_traits/add_lvalue_reference.h>
#include <__type_traits/conditional.h>
#include <__type_traits/enable_if.h>
#include <__type_traits/is_move_assignable.h>
#include <__type_traits/is_move_constructible.h>
#include <__type_traits/is_nothrow_move_assignable.h>
#include <__type_traits/is_nothrow_move_constructible.h>
#include <__type_traits/is_referenceable.h>
#include <__type_traits/is_same.h>
#include <__type_traits/is_void.h>
#include <__type_traits/nat.h>
#include <__utility/declval.h>
#include <cstddef>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

template <class _Tp> struct __is_swappable;
template <class _Tp> struct __is_nothrow_swappable;


#ifndef _LIBCPP_CXX03_LANG
template <class _Tp>
using __swap_result_t = typename enable_if<is_move_constructible<_Tp>::value && is_move_assignable<_Tp>::value>::type;
#else
template <class>
using __swap_result_t = void;
#endif

template <class _Tp>
inline _LIBCPP_INLINE_VISIBILITY
_LIBCPP_CONSTEXPR_SINCE_CXX20 __swap_result_t<_Tp>
swap(_Tp& __x, _Tp& __y) _NOEXCEPT_(is_nothrow_move_constructible<_Tp>::value &&
                                    is_nothrow_move_assignable<_Tp>::value);

template<class _Tp, size_t _Np>
inline _LIBCPP_INLINE_VISIBILITY _LIBCPP_CONSTEXPR_SINCE_CXX20
typename enable_if<
    __is_swappable<_Tp>::value
>::type
swap(_Tp (&__a)[_Np], _Tp (&__b)[_Np]) _NOEXCEPT_(__is_nothrow_swappable<_Tp>::value);

namespace __detail
{
// ALL generic swap overloads MUST already have a declaration available at this point.

template <class _Tp, class _Up = _Tp,
          bool _NotVoid = !is_void<_Tp>::value && !is_void<_Up>::value>
struct __swappable_with
{
    template <class _LHS, class _RHS>
    static decltype(swap(declval<_LHS>(), declval<_RHS>()))
    __test_swap(int);
    template <class, class>
    static __nat __test_swap(long);

    // Extra parens are needed for the C++03 definition of decltype.
    typedef decltype((__test_swap<_Tp, _Up>(0))) __swap1;
    typedef decltype((__test_swap<_Up, _Tp>(0))) __swap2;

    static const bool value = _IsNotSame<__swap1, __nat>::value
                           && _IsNotSame<__swap2, __nat>::value;
};

template <class _Tp, class _Up>
struct __swappable_with<_Tp, _Up,  false> : false_type {};

template <class _Tp, class _Up = _Tp, bool _Swappable = __swappable_with<_Tp, _Up>::value>
struct __nothrow_swappable_with {
  static const bool value =
#ifndef _LIBCPP_HAS_NO_NOEXCEPT
      noexcept(swap(declval<_Tp>(), declval<_Up>()))
  &&  noexcept(swap(declval<_Up>(), declval<_Tp>()));
#else
      false;
#endif
};

template <class _Tp, class _Up>
struct __nothrow_swappable_with<_Tp, _Up, false> : false_type {};

} // namespace __detail

template <class _Tp>
struct __is_swappable
    : public integral_constant<bool, __detail::__swappable_with<_Tp&>::value>
{
};

template <class _Tp>
struct __is_nothrow_swappable
    : public integral_constant<bool, __detail::__nothrow_swappable_with<_Tp&>::value>
{
};

#if _LIBCPP_STD_VER > 14

template <class _Tp, class _Up>
struct _LIBCPP_TEMPLATE_VIS is_swappable_with
    : public integral_constant<bool, __detail::__swappable_with<_Tp, _Up>::value>
{
};

template <class _Tp>
struct _LIBCPP_TEMPLATE_VIS is_swappable
    : public conditional<
        __is_referenceable<_Tp>::value,
        is_swappable_with<
            typename add_lvalue_reference<_Tp>::type,
            typename add_lvalue_reference<_Tp>::type>,
        false_type
    >::type
{
};

template <class _Tp, class _Up>
struct _LIBCPP_TEMPLATE_VIS is_nothrow_swappable_with
    : public integral_constant<bool, __detail::__nothrow_swappable_with<_Tp, _Up>::value>
{
};

template <class _Tp>
struct _LIBCPP_TEMPLATE_VIS is_nothrow_swappable
    : public conditional<
        __is_referenceable<_Tp>::value,
        is_nothrow_swappable_with<
            typename add_lvalue_reference<_Tp>::type,
            typename add_lvalue_reference<_Tp>::type>,
        false_type
    >::type
{
};

template <class _Tp, class _Up>
inline constexpr bool is_swappable_with_v = is_swappable_with<_Tp, _Up>::value;

template <class _Tp>
inline constexpr bool is_swappable_v = is_swappable<_Tp>::value;

template <class _Tp, class _Up>
inline constexpr bool is_nothrow_swappable_with_v = is_nothrow_swappable_with<_Tp, _Up>::value;

template <class _Tp>
inline constexpr bool is_nothrow_swappable_v = is_nothrow_swappable<_Tp>::value;

#endif // _LIBCPP_STD_VER > 14

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___TYPE_TRAITS_IS_SWAPPABLE_H
