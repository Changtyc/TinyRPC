#pragma once
#ifndef TINY_RPC_IO_SERVICE_POOL_H_
#define TINY_RPC_IO_SERVICE_POOL_H_

#include <vector>
#include <memory>
#include <boost/asio.hpp>

class io_service_pool : private boost::asio::noncopyable {
public:
	explicit io_service_pool(std::size_t pool_size) : next_io_service_(0) {
		if (pool_size == 0) throw std::runtime_error("io_service_pool size is 0");

		for (std::size_t i = 0; i < pool_size; ++i) {
			io_service_ptr io_service(new boost::asio::io_service);
			work_ptr work(new boost::asio::io_service::work(*io_service));
			io_services_.push_back(io_service);
			work_.push_back(work);
		}

	}

	void run() {
		// 把所有的io事件放进子线程进行监听
		std::vector<std::shared_ptr<std::thread>> threads;
		for (std::size_t i = 0; i < io_services_.size(); ++i) {
			threads.emplace_back(
				std::make_shared<std::thread>([](io_service_ptr svr) { svr->run(); }, io_services_[i]));
		}

		for (std::size_t i = 0; i < threads.size(); ++i) threads[i]->join();
	}

	void stop() {
		for (std::size_t i = 0; i < io_services_.size(); ++i) {
			io_services_[i]->stop();
		}
	}

	// 循环返回引用
	boost::asio::io_service& get_io_service() {
		boost::asio::io_service& io_service = *io_services_[next_io_service_];
		++next_io_service_;
		if (next_io_service_ == io_services_.size()) next_io_service_ = 0;
		return io_service;
	}

private:
	typedef std::shared_ptr<boost::asio::io_service> io_service_ptr;
	typedef std::shared_ptr<boost::asio::io_service::work> work_ptr;

	// io_service 池，循环池
	std::vector<io_service_ptr> io_services_;

	// io_service::work 池，保证io_service一直能工作
	std::vector<work_ptr> work_;

	// 下一个io_service的序号
	std::size_t next_io_service_;
};


#endif