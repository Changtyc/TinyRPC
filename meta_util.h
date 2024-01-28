#pragma once
#ifndef TINY_RPC_META_H_
#define TINY_RPC_META_H_

#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

// ��������������ȡģ��

namespace meta_util {
	template<typename T>
	using remove_const_reference_t = std::remove_const_t<std::remove_reference_t<T>>;

	template <typename T>
	struct function_traits;

	// ͨ�ú�������Ӷ�һ��string����ʾ������
	template <typename Ret, typename... Args>
	struct function_traits<Ret(Args...)> {
		enum { arity = sizeof...(Args) };
		using return_type = Ret;
		using stl_function_type = std::function<Ret(Args...)>;
		using pointer = Ret(*)(Args...);
		using args_tuple = std::tuple<std::string, std::remove_const_t<std::remove_reference_t<Args>>...>;
	};

	// �����ػ�����һ������
	template<typename Ret, typename Arg, typename... Args>
	struct function_traits<Ret(Arg, Args...)>
	{
		enum { arity = sizeof...(Args) + 1 };
		using return_type = Ret;
		using stl_function_type = std::function<Ret(Arg, Args...)>;
		using pointer = Ret(*)(Arg, Args...);
		using args_tuple = std::tuple<std::string, Arg, std::remove_const_t<std::remove_reference_t<Args>>...>;
	};

	// �����ػ����޲���
	template<typename Ret>
	struct function_traits<Ret()> {
	public:
		enum { arity = 0 };
		using return_type = Ret;
		using stl_function_type = std::function<Ret()>;
		using pointer = Ret(*)();
		using args_tuple = std::tuple<std::string>;
	};

	//�����ػ�������ָ��
	template <typename Ret, typename... Args>
	struct function_traits<Ret(*)(Args...)> : function_traits<Ret(Args...)> {};


	//�����ػ���std::function
	template <typename Ret, typename... Args>
	struct function_traits<std::function<Ret(Args...)>> : function_traits<Ret(Args...)> {};

	//�����ػ�����Ա����ָ��
	template <typename ReturnType, typename ClassType, typename... Args>
	struct function_traits<ReturnType(ClassType::*)(Args...)> : function_traits<ReturnType(Args...)> {};

	//�����ػ���const ��Ա����ָ��
	template <typename ReturnType, typename ClassType, typename... Args>
	struct function_traits<ReturnType(ClassType::*)(Args...) const> : function_traits<ReturnType(Args...)> {};

	// �����ػ�����������
	template<typename Callable>
	struct function_traits : function_traits<decltype(&Callable::operator())> {};


	template <int N, typename... Args>
	using nth_type_of = std::tuple_element_t<N, std::tuple<Args...>>;

	template <typename... Args>
	using last_type_of = nth_type_of<sizeof...(Args) - 1, Args...>;


	// ��������ģ�壺��ӡ��������
	template <typename Tuple, std::size_t Index = 0>
	void printArgsTupleTypes() {
		if constexpr (Index < std::tuple_size_v<Tuple>) {
			std::cout << "Type at index " << Index << ": "
				<< typeid(std::tuple_element_t<Index, Tuple>).name() << std::endl;
			printArgsTupleTypes<Tuple, Index + 1>();
		}
	}
}

#endif