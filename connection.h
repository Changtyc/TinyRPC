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

// 协议常量
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

enum class request_type : uint8_t { req_res, sub_pub };

struct message_type {
    std::uint64_t req_id;
    request_type req_type;
    std::shared_ptr<std::string> content;
};

// 13个字节,但因为字节对齐，拓展为24字节
struct rpc_header {
    uint32_t body_len;
    uint64_t req_id;
    request_type req_type;
};

static const size_t MAX_BUF_LEN = 1048576 * 10;
static const size_t HEAD_LEN = 13;
static const size_t INIT_BUF_SIZE = 2 * 1024;

/*
* 连接类
 通过继承自 std::enable_shared_from_this，
 该类可以安全地生成指向自身的std::shared_ptr<connection>对象,
 防止在异步操作执行过程中对象被析构。
*/
class connection : public std::enable_shared_from_this<connection>,
                   private boost::asio::noncopyable {
  public:
    connection(boost::asio::io_service &io_service, std::size_t timeout_seconds,
               std::shared_ptr<std::unordered_map<
                   std::string,
                   std::function<void(const char *, size_t, std::string &)>>>
                   ptr)
        : socket_(io_service), timer_(io_service), body_(INIT_BUF_SIZE),
          timeout_seconds_(timeout_seconds), m_sharedMapPtr_(ptr),
          has_closed_(false) {
        conn_id_ = 0;
        memset(head_, 0, sizeof(head_));
    }

    ~connection() { close(); }

    // 返回连接对于的socket
    boost::asio::ip::tcp::socket &socket() { return socket_; }

    // 返回连接id
    void set_conn_id(int64_t id) { conn_id_ = id; }

    // 返回连接是否已经关闭
    bool has_closed() const { return has_closed_; }

    // 开始连接，外部接口，接收信息，返回调用
    void start() {
        // 递归读取请求头，接收连接
        read_header();
    }

    // 消息的解析系列函数
  private:
    // 解析消息头
    void read_header() {
        // 重置定时器
        reset_timer();
        // 确保对象在异步操作完成之前不会被销毁
        auto self(this->shared_from_this());
        boost::asio::async_read(
            socket_, boost::asio::buffer(head_, HEAD_LEN),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!socket_.is_open())
                    return;
                if (!ec) {
                    uint32_t body_len = 0;
                    memcpy(&body_len, head_, 4);
                    memcpy(&req_id_, head_ + 4, 8);
                    memcpy(&req_type_, head_ + 12, 1);

                    if (body_len > 0 && body_len < MAX_BUF_LEN) {
                        if (body_.size() < body_len) {
                            body_.resize(body_len);
                        }
                        read_body(body_len);
                        return;
                    }
                    if (body_len == 0) {
                        // 删除定时，重新等待通信
                        cancel_timer();
                        read_header();
                        return;
                    }
                } else {
                    // 出错了断开连接
                    close();
                    return;
                }
            });
    }

    // 解析消息体
    void read_body(std::size_t size) {
        auto self(this->shared_from_this());
        boost::asio::async_read(
            socket_, boost::asio::buffer(body_.data(), size),
            [this, self](boost::system::error_code ec, std::size_t length) {
                // 取消定时，避免超时断开连接
                cancel_timer();
                if (!socket_.is_open()) {
                    return;
                }
                if (!ec) {
                    // 递归读，等待下一次调用，备份消息避免两次消息对冲混淆
                    request_type tmp_req_type = req_type_;
                    std::string tmp_body(body_.data(), length);
                    read_header();
                    if (req_type_ == request_type::req_res) {
                        route(tmp_body.data(), length); // 调用函数
                    } else {
                        // 返回错误信息
                    }
                } else {
                    // 出错了断开连接
                    close();
                    return;
                }
            });
    }

    // 处理信息，路由调用函数
    void route(const char *data, std::size_t size) {
        std::string result;
        std::uint64_t reqid = req_id_;

        RPCbufferPack::msgpack_codec codec;
        std::tuple<std::string> p =
            codec.unpack<std::tuple<std::string>>(data, size);
        // 得到函数名
        std::string func_name = std::get<0>(p);

        auto it = m_sharedMapPtr_->find(func_name);
        if (it == m_sharedMapPtr_->end()) {
            result = codec.pack_args_str(result_code::FAIL,
                                         "unknown function: " + func_name);
        } else {
            // 调用函数
            it->second(data, size, result);
        }
        // 写回操作
        response(req_id_, std::move(result));
    }

    /*写回操作的系列函数*/
  private:
    void response(uint64_t req_id, std::string data,
                  request_type req_type = request_type::req_res) {
        auto len = data.size();
        assert(len < MAX_BUF_LEN);

        // async_write
        // 不能同时写两次，保证第一次写完再写第二次，否则会乱码，这也是write_queue_的作用
        {
            std::unique_lock<std::mutex> lock(write_mtx_);
            write_queue_.emplace_back(
                message_type{req_id, req_type,
                             std::make_shared<std::string>(std::move(data))});
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
        auto &msg = write_queue_.front();
        uint32_t sendsz = msg.content->size();
        std::array<boost::asio::const_buffer, 4> write_buffers;
        write_buffers[0] = boost::asio::buffer(&sendsz, sizeof(uint32_t));
        write_buffers[1] = boost::asio::buffer(&msg.req_id, sizeof(uint64_t));
        write_buffers[2] =
            boost::asio::buffer(&msg.req_type, sizeof(request_type));
        write_buffers[3] = boost::asio::buffer(msg.content->data(), sendsz);

        auto self = this->shared_from_this();
        boost::asio::async_write(
            socket_, write_buffers,
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    std::cout
                        << "Write completed. Bytes transferred: " << length
                        << std::endl;
                }
                do_write(ec, length);
            });
    }

    void do_write(boost::system::error_code ec, std::size_t length) {
        if (ec) {
            close();
            return;
        }
        if (has_closed()) {
            return;
        }
        std::unique_lock<std::mutex> lock(write_mtx_);
        write_queue_.pop_front();
        // 循环发送
        if (!write_queue_.empty()) {
            write();
        } else {
            is_write_ = false;
        }
    }

  private:
    // 重置定时器，回调函数为删除连接
    void reset_timer() {
        if (timeout_seconds_ == 0) {
            return;
        }
        auto self(this->shared_from_this());
        timer_.expires_from_now(std::chrono::seconds(timeout_seconds_));
        timer_.async_wait([this, self](const boost::system::error_code &ec) {
            if (has_closed()) {
                return;
            }
            if (ec) {
                return;
            }
            close();
        });
    }

    // 断开当前连接
    void close() {
        if (has_closed_) {
            return;
        }
        boost::system::error_code ignored_ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                         ignored_ec);
        socket_.close(ignored_ec);
        has_closed_ = true;
    }

    // 取消定时器
    void cancel_timer() {
        // 一般调用处理函数的时候使用，避免调用过程中连接断开
        if (timeout_seconds_ == 0) {
            return;
        }
        timer_.cancel();
    }

  private:
    boost::asio::ip::tcp::socket socket_;
    int64_t conn_id_ = 0;             // 连接类id
    boost::asio::steady_timer timer_; // 定时器
    std::size_t timeout_seconds_;     // 超时时间
    bool has_closed_;                 // 连接断开标志

    // 存疑，以下变量在运行过程会随着同一个连接的多个请求而变换
    char head_[HEAD_LEN];    // 消息头
    std::vector<char> body_; // 消息体
    std::uint64_t req_id_;   // 请求id
    request_type req_type_;  // 请求类型

    std::mutex write_mtx_;
    std::deque<message_type> write_queue_;
    bool is_write_ = false;

    // 函数映射表指针-传输数据，数据长度，返回结果
    std::shared_ptr<std::unordered_map<
        std::string, std::function<void(const char *, size_t, std::string &)>>>
        m_sharedMapPtr_;
};

#endif