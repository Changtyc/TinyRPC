#pragma once
#ifndef TINY_RPC_CONNECTION_H_
#define TINY_RPC_CONNECTION_H_

#include <iostream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <array>
#include <deque>
#include <boost/asio.hpp>
#include "codec.h"
#include "meta_util.h"


// Э�鳣��
enum class result_code : int {
	OK = 0,
	FAIL = 1,
};

enum class error_code {
	OK,
	UNKNOWN,
	FAIL,
	TIMEOUT,
	CANCEL,
	BADCONNECTION,
};

enum class request_type : uint8_t {
	req_res,
	sub_pub
};

struct message_type {
	std::uint64_t req_id;
	request_type req_type;
	std::shared_ptr<std::string> content;
};

// 13���ֽ�,����Ϊ�ֽڶ��룬��չΪ24�ֽ�
struct rpc_header {
	uint32_t body_len;
	uint64_t req_id;
	request_type req_type;
};


static const size_t MAX_BUF_LEN = 1048576 * 10;
static const size_t HEAD_LEN = 13;
static const size_t INIT_BUF_SIZE = 2 * 1024;

/*
* ������
 ͨ���̳��� std::enable_shared_from_this��
 ������԰�ȫ������ָ�������std::shared_ptr<connection>����,
 ��ֹ���첽����ִ�й����ж���������
*/
class connection : public std::enable_shared_from_this<connection>, private boost::asio::noncopyable {
public:
	connection(boost::asio::io_service& io_service, std::size_t timeout_seconds,
		std::shared_ptr<std::unordered_map<std::string, std::function<void(const char*, size_t, std::string&)>>> ptr) :
		socket_(io_service),
		timer_(io_service),
		body_(INIT_BUF_SIZE),
		timeout_seconds_(timeout_seconds),
		m_sharedMapPtr_(ptr),
		has_closed_(false) {
		conn_id_ = 0;
		memset(head_, 0, sizeof(head_));
	}

	~connection() {
		close();
	}

	// �������Ӷ��ڵ�socket
	boost::asio::ip::tcp::socket& socket() { return socket_; }

	// ��������id
	void set_conn_id(int64_t id) { conn_id_ = id; }

	// ���������Ƿ��Ѿ��ر�
	bool has_closed() const { return has_closed_; }

	// ��ʼ���ӣ��ⲿ�ӿڣ�������Ϣ�����ص���
	void start() {
		// �ݹ��ȡ����ͷ����������
		read_header();
	}

	// ��Ϣ�Ľ���ϵ�к���
private:
	// ������Ϣͷ
	void read_header() {
		//���ö�ʱ��
		reset_timer();
		// ȷ���������첽�������֮ǰ���ᱻ����
		auto self(this->shared_from_this());
		boost::asio::async_read(socket_, boost::asio::buffer(head_, HEAD_LEN),
			[this, self](boost::system::error_code ec, std::size_t length) {
				if (!socket_.is_open()) return;
				if (!ec) {
					uint32_t body_len = 0;
					memcpy(&body_len, head_, 4);
					memcpy(&req_id_, head_ + 4, 8);
					memcpy(&req_type_, head_ + 12, 1);

					if (body_len > 0 && body_len < MAX_BUF_LEN) {
						if (body_.size() < body_len) { body_.resize(body_len); }
						read_body(body_len);
						return;
					}
					if (body_len == 0) {
						// ɾ����ʱ�����µȴ�ͨ��
						cancel_timer();
						read_header();
						return;
					}
				}
				else {
					// �����˶Ͽ�����
					close();
					return;
				}
			});

	}

	// ������Ϣ��
	void read_body(std::size_t size) {
		auto self(this->shared_from_this());
		boost::asio::async_read(socket_, boost::asio::buffer(body_.data(), size),
			[this, self](boost::system::error_code ec, std::size_t length) {
				// ȡ����ʱ�����ⳬʱ�Ͽ�����
				cancel_timer();
				if (!socket_.is_open()) {
					return;
				}
				if (!ec) {
					// �ݹ�����ȴ���һ�ε��ã�������Ϣ����������Ϣ�Գ����
					request_type tmp_req_type = req_type_;
					std::string tmp_body(body_.data(), length);
					read_header();
					if (req_type_ == request_type::req_res) {
						route(tmp_body.data(), length);// ���ú���
					}
					else {
						// ���ش�����Ϣ
					}
				}
				else {
					// �����˶Ͽ�����
					close();
					return;
				}
			});
	}

	// ������Ϣ��·�ɵ��ú���
	void route(const char* data, std::size_t size) {
		std::string result;
		std::uint64_t reqid = req_id_;

		RPCbufferPack::msgpack_codec codec;
		std::tuple<std::string> p = codec.unpack<std::tuple<std::string>>(data, size);
		// �õ�������
		std::string func_name = std::get<0>(p);

		auto it = m_sharedMapPtr_->find(func_name);
		if (it == m_sharedMapPtr_->end()) {
			result = codec.pack_args_str(result_code::FAIL, "unknown function: " + func_name);
		}
		else {
			// ���ú���
			it->second(data, size, result);
		}
		// д�ز���
		response(req_id_, std::move(result));
	}

	/*д�ز�����ϵ�к���*/
private:
	void response(uint64_t req_id, std::string data, request_type req_type = request_type::req_res) {
		auto len = data.size();
		assert(len < MAX_BUF_LEN);

		// async_write ����ͬʱд���Σ���֤��һ��д����д�ڶ��Σ���������룬��Ҳ��write_queue_������
		{
			std::unique_lock<std::mutex> lock(write_mtx_);
			write_queue_.emplace_back(message_type{ req_id, req_type, std::make_shared<std::string>(std::move(data)) });
		}

		if (!is_write_) {
			{
				std::unique_lock<std::mutex> lock(write_mtx_);
				is_write_ = true;
				write();
			}
		}
	}

	void write() {
		auto& msg = write_queue_.front();
		uint32_t sendsz = msg.content->size();
		std::array<boost::asio::const_buffer, 4> write_buffers;
		write_buffers[0] = boost::asio::buffer(&sendsz, sizeof(uint32_t));
		write_buffers[1] = boost::asio::buffer(&msg.req_id, sizeof(uint64_t));
		write_buffers[2] = boost::asio::buffer(&msg.req_type, sizeof(request_type));
		write_buffers[3] = boost::asio::buffer(msg.content->data(), sendsz);

		auto self = this->shared_from_this();
		boost::asio::async_write(socket_, write_buffers, [this, self](boost::system::error_code ec, std::size_t length) {
			if (!ec) {
				std::cout << "Write completed. Bytes transferred: " << length << std::endl;
			}
			do_write(ec, length);
			});
	}

	void do_write(boost::system::error_code ec, std::size_t length) {
		if (ec) {
			close();
			return;
		}
		if (has_closed()) { return; }
		std::unique_lock<std::mutex> lock(write_mtx_);
		write_queue_.pop_front();
		// ѭ������
		if (!write_queue_.empty()) {
			write();
		}
		else {
			is_write_ = false;
		}
	}


private:
	// ���ö�ʱ�����ص�����Ϊɾ������
	void reset_timer() {
		if (timeout_seconds_ == 0) { return; }
		auto self(this->shared_from_this());
		timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
		timer_.async_wait([this, self](const boost::system::error_code& ec) {
			if (has_closed()) { return; }
			if (ec) { return; }
			close();
			});
	}

	// �Ͽ���ǰ����
	void close() {
		if (has_closed_) {
			return;
		}
		boost::system::error_code ignored_ec;
		socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
		socket_.close(ignored_ec);
		has_closed_ = true;
	}

	// ȡ����ʱ��
	void cancel_timer() {
		// һ����ô�������ʱ��ʹ�ã�������ù��������ӶϿ�
		if (timeout_seconds_ == 0) { return; }
		timer_.cancel();
	}


private:
	boost::asio::ip::tcp::socket socket_;
	int64_t conn_id_ = 0; //������id
	boost::asio::steady_timer timer_; // ��ʱ��
	std::size_t timeout_seconds_; // ��ʱʱ��
	bool has_closed_; // ���ӶϿ���־

	// ���ɣ����±��������й��̻�����ͬһ�����ӵĶ��������任
	char head_[HEAD_LEN];// ��Ϣͷ
	std::vector<char> body_; // ��Ϣ��
	std::uint64_t req_id_; // ����id
	request_type req_type_; // ��������


	std::mutex write_mtx_;
	std::deque<message_type> write_queue_;
	bool is_write_ = false;

	// ����ӳ���ָ��-�������ݣ����ݳ��ȣ����ؽ��
	std::shared_ptr<std::unordered_map<std::string, std::function<void(const char*, size_t, std::string&)>>> m_sharedMapPtr_;
};


#endif