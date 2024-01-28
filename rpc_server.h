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


// ����ģʽ���ɸ���
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
		// ��ʼ��ע�ắ����ָ��
		sharedMapPtr_ = std::make_shared<std::unordered_map<std::string, std::function<void(const char*, size_t, std::string&)>>>();
		// ��ʼ�ݹ�ȴ�����
		do_accept();
		// �����߳���������ɾ����ʱ����,���ٿռ�ռ��
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

	// ��ʼ���񣬴������̼߳���io_service
	void run() {
		io_service_pool_.run();
	}

	// ����ע��
	template<typename Function>
	void register_handler(std::string const& name, const Function& f) {
		// ע�ắ��
		register_nonmember_func(name, std::move(f));
	}



private:
	// �����첽�������Ӳ���
	void do_accept() {
		// ����ָ������Ȩ
		conn_.reset(new connection(io_service_pool_.get_io_service(), timeout_seconds_, sharedMapPtr_));
		// �첽�ȴ�����,ʹ��lambda���ʽ
		acceptor_.async_accept(conn_->socket(), [this](boost::system::error_code ec)->void {
			if (ec) {
				return;
			}

			// ��ȡ���ӵ�Զ�̶˵���Ϣ
			boost::asio::ip::tcp::endpoint remoteEndpoint = conn_->socket().remote_endpoint();

			// ������ӵ� IP �Ͷ˿ں�
			std::cout << "Accepted connection from: " << remoteEndpoint.address().to_string()
				<< ":" << remoteEndpoint.port() << std::endl;

			// ���ӵĶ�ȡ
			conn_->start();

			// ������ӱ�ţ�ʹ���ھֲ����������
			{
				std::unique_lock<std::mutex> lock(mtx_);
				conn_->set_conn_id(conn_id_);
				connections_[conn_id_++] = conn_;
			}
			// �ݹ�����������Ͻ����µ�����
			do_accept();
			});
	}

	// ӳ����ɾ����ʱ���ӣ�����ռ�����
	void clean() {
		while (!stop_check_) {
			std::unique_lock<std::mutex> lock(mtx_);
			// ����Ҫ�������һ�ֶ�ʱ���ƣ���ʱɾ���ԶϿ�������
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


	/*Զ�̹��̵ĵ��õ�ϵ�к���,ע���ػ���һ��Ϊstring*/
private:
	template <typename Function, size_t... Indices, typename Arg, typename... Args>
	static typename std::result_of<Function(Args...)>::type call_helper(
		const Function& f, const std::index_sequence<Indices...>&, std::tuple<Arg, Args...> tup) {
		return f(std::move(std::get<Indices + 1>(tup))...);
	}

	// ���������� void �ĺ������á�
	template <typename Function, typename Arg, typename... Args>
	static typename std::enable_if<std::is_void<typename std::result_of<Function(Args...)>::type>::value>::type
		call(const Function& f, std::string& result, std::tuple<Arg, Args...> tp) {
		call_helper(f, std::make_index_sequence<sizeof...(Args)>{}, std::move(tp));
		RPCbufferPack::msgpack_codec codec;
		result = RPCbufferPack::msgpack_codec::pack_args_str(result_code::OK);
	}


	// ���������ͷ� void �ĺ������á�
	template <typename Function, typename Arg, typename... Args>
	static typename std::enable_if<!std::is_void<typename std::result_of<Function(Args...)>::type>::value>::type
		call(const Function& f, std::string& result, std::tuple<Arg, Args...> tp) {
		auto r = call_helper(f, std::make_index_sequence<sizeof...(Args)>{}, std::move(tp));
		RPCbufferPack::msgpack_codec codec;
		result = RPCbufferPack::msgpack_codec::pack_args_str(result_code::OK, r);
	}


	template<typename Function>
	struct invoker {
		// result �Ƿ��ؽ���ַ���
		static inline void apply(const Function& func, const char* data, size_t size,
			std::string& result) {
			// ����һ��string����
			using argstuple = typename meta_util::function_traits<Function>::args_tuple;
			RPCbufferPack::msgpack_codec codec;
			try {
				auto tp = codec.unpack<argstuple>(data, size);
				// ʹ��ģ�����==���ﱨ����
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

	// ע�ắ��,ʹ��lambda�����µĺ���
	template<typename Function>
	void register_nonmember_func(std::string const& name, Function f) {
		(*sharedMapPtr_)[name] = [f](const char* data, size_t size, std::string& result) {
			invoker<Function>::apply(std::move(f), data, size, result);
		};
	}



private:
	io_service_pool io_service_pool_; // io�¼���
	boost::asio::ip::tcp::acceptor acceptor_; // tcp������
	std::shared_ptr<connection> conn_; // �������ָ��
	//std::shared_ptr<std::thread> thd_; // �첽ִ�е��߳�
	std::size_t timeout_seconds_;// ��ʱ���ӵ�ʱ��

	// �������ñ���
	int64_t conn_id_ = 0;
	std::unordered_map<int64_t, std::shared_ptr<connection>> connections_; // �������ӳ��
	std::shared_ptr<std::thread> check_thread_;// ��ʱ������߳�
	size_t check_seconds_;// ��ʱ�����ʱ��
	bool stop_check_ = false;
	std::mutex mtx_; // ������
	std::condition_variable cv_; // ��������

	// ����ӳ���ָ�룬��ÿ��connection����
	std::shared_ptr<std::unordered_map<std::string, std::function<void(const char*, size_t, std::string&)>>> sharedMapPtr_;

};


#endif