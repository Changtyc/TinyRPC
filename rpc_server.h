#pragma once
#ifndef TINY_RPC_SERVER_H_
#define TINY_RPC_SERVER_H_

#include <boost/asio.hpp>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <condition_variable>
#include "connection.h"
#include "io_service_pool.h"


// 单例模式不可复制
class rpc_server : private boost::asio::noncopyable {
public:
	rpc_server(unsigned short port, size_t size, size_t timeout_seconds = 15, size_t check_seconds = 10) :
		io_service_pool_(size),
		acceptor_(io_service_pool_.get_io_service(), boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
		timeout_seconds_(timeout_seconds),
		check_seconds_(check_seconds)
	{
		stop_check_ = false;
		conn_id_ = 0;
		// 初始化注册函数表指针
		sharedMapPtr_ = std::make_shared<std::unordered_map<std::string, std::function<void(const char*, size_t, std::string&)>>>();
		// 开始递归等待连接
		do_accept();
		// 启动线程用于清理删除超时连接,减少空间占用
		check_thread_ = std::make_shared<std::thread>([this] { this->clean(); });
	};


	~rpc_server() {
		{
			std::unique_lock<std::mutex> lock(mtx_);
			stop_check_ = true;
			cv_.notify_all();
		}
		check_thread_->join();
		io_service_pool_.stop();
	}

	// 开始服务，创建子线程监听io_service
	void run() {
		io_service_pool_.run();
	}

	// 函数注册
	template<typename Function>
	void register_handler(std::string const& name, const Function& f) {
		// 注册函数
		register_nonmember_func(name, std::move(f));
	}



private:
	// 启动异步接受连接操作
	void do_accept() {
		// 重置指针所有权
		conn_.reset(new connection(io_service_pool_.get_io_service(), timeout_seconds_, sharedMapPtr_));
		// 异步等待连接,使用lambda表达式
		acceptor_.async_accept(conn_->socket(), [this](boost::system::error_code ec)->void {
			if (ec) {
				return;
			}

			// 获取连接的远程端点信息
			boost::asio::ip::tcp::endpoint remoteEndpoint = conn_->socket().remote_endpoint();

			// 输出连接的 IP 和端口号
			std::cout << "Accepted connection from: " << remoteEndpoint.address().to_string()
				<< ":" << remoteEndpoint.port() << std::endl;

			// 连接的读取
			conn_->start();

			// 添加连接编号，使用在局部域避免死锁
			{
				std::unique_lock<std::mutex> lock(mtx_);
				conn_->set_conn_id(conn_id_);
				connections_[conn_id_++] = conn_;
			}
			// 递归调用自身，不断接收新的连接
			do_accept();
			});
	}

	// 映射中删除超时连接，避免空间膨胀
	void clean() {
		while (!stop_check_) {
			std::unique_lock<std::mutex> lock(mtx_);
			// 更重要的是提高一种定时机制，定时删除以断开的连接
			cv_.wait_for(lock, std::chrono::seconds(check_seconds_));
			for (auto it = connections_.begin(); it != connections_.end();) {
				if (it->second->has_closed()) {
					it = connections_.erase(it);
				}
				else {
					++it;
				}
			}
		}
	}


	/*远程过程的调用的系列函数,注意特化第一个为string*/
private:
	template <typename Function, size_t... Indices, typename Arg, typename... Args>
	static typename std::result_of<Function(Args...)>::type call_helper(
		const Function& f, const std::index_sequence<Indices...>&, std::tuple<Arg, Args...> tup) {
		return f(std::move(std::get<Indices + 1>(tup))...);
	}

	// 处理返回类型 void 的函数调用。
	template <typename Function, typename Arg, typename... Args>
	static typename std::enable_if<std::is_void<typename std::result_of<Function(Args...)>::type>::value>::type
		call(const Function& f, std::string& result, std::tuple<Arg, Args...> tp) {
		call_helper(f, std::make_index_sequence<sizeof...(Args)>{}, std::move(tp));
		RPCbufferPack::msgpack_codec codec;
		result = RPCbufferPack::msgpack_codec::pack_args_str(result_code::OK);
	}


	// 处理返回类型非 void 的函数调用。
	template <typename Function, typename Arg, typename... Args>
	static typename std::enable_if<!std::is_void<typename std::result_of<Function(Args...)>::type>::value>::type
		call(const Function& f, std::string& result, std::tuple<Arg, Args...> tp) {
		auto r = call_helper(f, std::make_index_sequence<sizeof...(Args)>{}, std::move(tp));
		RPCbufferPack::msgpack_codec codec;
		result = RPCbufferPack::msgpack_codec::pack_args_str(result_code::OK, r);
	}


	template<typename Function>
	struct invoker {
		// result 是返回结果字符串
		static inline void apply(const Function& func, const char* data, size_t size,
			std::string& result) {
			// 多了一个string参数
			using argstuple = typename meta_util::function_traits<Function>::args_tuple;
			RPCbufferPack::msgpack_codec codec;
			try {
				auto tp = codec.unpack<argstuple>(data, size);
				// 使用模板调用==这里报错了
				call(func, result, std::move(tp));
			}
			catch (std::invalid_argument& e) {
				result = codec.pack_args_str(result_code::FAIL, e.what());
			}
			catch (const std::exception& e) {
				result = codec.pack_args_str(result_code::FAIL, e.what());
			}

		}
	};

	// 注册函数,使用lambda创建新的函数
	template<typename Function>
	void register_nonmember_func(std::string const& name, Function f) {
		(*sharedMapPtr_)[name] = [f](const char* data, size_t size, std::string& result) {
			invoker<Function>::apply(std::move(f), data, size, result);
		};
	}



private:
	io_service_pool io_service_pool_; // io事件池
	boost::asio::ip::tcp::acceptor acceptor_; // tcp接收器
	std::shared_ptr<connection> conn_; // 连接类的指针
	//std::shared_ptr<std::thread> thd_; // 异步执行的线程
	std::size_t timeout_seconds_;// 超时连接的时间

	// 连接所用变量
	int64_t conn_id_ = 0;
	std::unordered_map<int64_t, std::shared_ptr<connection>> connections_; // 连接序号映射
	std::shared_ptr<std::thread> check_thread_;// 定时清理的线程
	size_t check_seconds_;// 定时清理的时间
	bool stop_check_ = false;
	std::mutex mtx_; // 互斥锁
	std::condition_variable cv_; // 条件变量

	// 函数映射表指针，和每个connection共享
	std::shared_ptr<std::unordered_map<std::string, std::function<void(const char*, size_t, std::string&)>>> sharedMapPtr_;

};


#endif