#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "Krpcmsgpack.h"

namespace krpc {

namespace detail {

template <std::size_t... I>
struct index_sequence { };

template <std::size_t N, std::size_t... I>
struct make_index_sequence : make_index_sequence<N - 1, N - 1, I...> { };

template <std::size_t... I>
struct make_index_sequence<0, I...> { using type = index_sequence<I...>; };

template <typename F, typename Tuple, std::size_t... I>
inline auto call_impl(F &&f, Tuple &&t, index_sequence<I...>)
    -> decltype(std::forward<F>(f)(std::get<I>(std::forward<Tuple>(t))...)) {
    return std::forward<F>(f)(std::get<I>(std::forward<Tuple>(t))...);
}

template <typename F, typename Tuple>
inline auto call(F &&f, Tuple &&t)
    -> decltype(call_impl(std::forward<F>(f), std::forward<Tuple>(t),
                          typename make_index_sequence<std::tuple_size<typename std::decay<Tuple>::type>::value>::type{})) {
    return call_impl(std::forward<F>(f), std::forward<Tuple>(t),
                     typename make_index_sequence<std::tuple_size<typename std::decay<Tuple>::type>::value>::type{});
}

template <typename T>
struct func_traits;

template <typename R, typename... Args>
struct func_traits<R(Args...)> {
    using result_type = R;
    using args_type = std::tuple<typename std::decay<Args>::type...>;
};

template <typename R, typename... Args>
struct func_traits<R (*)(Args...)> : func_traits<R(Args...)> { };

template <typename C, typename R, typename... Args>
struct func_traits<R (C::*)(Args...)> : func_traits<R(Args...)> { };

template <typename C, typename R, typename... Args>
struct func_traits<R (C::*)(Args...) const> : func_traits<R(Args...)> { };

template <typename F>
struct func_traits : func_traits<decltype(&F::operator())> { };

} // namespace detail

class MsgpackDispatcher {
public:
    template <typename F>
    void bind(const std::string &service, const std::string &method, F func) {
        using traits = detail::func_traits<F>;
        using args_type = typename traits::args_type;
        using result_type = typename traits::result_type;

        const std::string key = service + "." + method;
        handlers_.emplace(key, HandlerBuilder<F, args_type, result_type>::Make(func));
    }

    bool Dispatch(const std::string &service,
                  const std::string &method,
                  const krpc::msgpack::object &args,
                  krpc::msgpack::object_handle *result,
                  std::string *error) {
        const std::string key = service + "." + method;
        auto it = handlers_.find(key);
        if (it == handlers_.end()) {
            if (error) {
                *error = "no such method: " + key;
            }
            return false;
        }

        try {
            if (result) {
                *result = it->second(args);
            }
            return true;
        } catch (const std::exception &e) {
            if (error) {
                *error = std::string("call_error: ") + e.what();
            }
            return true;
        } catch (...) {
            if (error) {
                *error = "call_error: unknown";
            }
            return true;
        }
    }

private:
    using handler_t = std::function<krpc::msgpack::object_handle(const krpc::msgpack::object &)>;

    template <typename T>
    static krpc::msgpack::object_handle MakeHandle(T &&value) {
        auto z = krpc::msgpack::unique_ptr<krpc::msgpack::zone>(new krpc::msgpack::zone());
        krpc::msgpack::object obj(std::forward<T>(value), *z);
        return krpc::msgpack::object_handle(obj, std::move(z));
    }

    static krpc::msgpack::object_handle MakeNil() {
        return MakeHandle(krpc::msgpack::type::nil_t());
    }

    template <typename F, typename ArgsTuple, typename Result>
    struct HandlerBuilder {
        static handler_t Make(F func) {
            return [func](const krpc::msgpack::object &args) {
                ArgsTuple args_real;
                args.convert(args_real);
                auto result = detail::call(func, args_real);
                return MakeHandle(std::move(result));
            };
        }
    };

    template <typename F, typename ArgsTuple>
    struct HandlerBuilder<F, ArgsTuple, void> {
        static handler_t Make(F func) {
            return [func](const krpc::msgpack::object &args) {
                ArgsTuple args_real;
                args.convert(args_real);
                detail::call(func, args_real);
                return MakeNil();
            };
        }
    };

    std::unordered_map<std::string, handler_t> handlers_;
};

} // namespace krpc

