#pragma once
#ifndef TINY_RPC_CLIENT_H_
#define TINY_RPC_CLIENT_H_

#include <boost/asio.hpp>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <future>
#include <condition_variable>
#include "connection.h"

const constexpr size_t DEFAULT_TIMEOUT = 5000; // milliseconds

class rpc_client : private boost::asio::noncopyable {
  public:
    rpc_client(const std::string &host, unsigned short port)
        : socket_(ioservice_), work_(ioservice_), host_(host), port_(port),
          body_(INIT_BUF_SIZE) {
        has_connected_ = false;
        conn_val = false;
        write_fg_ = false;
        m_req_id = 0;
        // 创建子线程
        thd_ = std::make_shared<std::thread>([this] { ioservice_.run(); });
        // 创建写子线程
        write_thd_ =
            std::make_shared<std::thread>([this] { write_callback(); });
    }

    ~rpc_client() {
        close();
        stop();
    }

    // 开始连接
    bool connect(size_t timeout = 3) {
        if (has_connected_)
            return true;
        assert(port_ != 0);
        auto addr = boost::asio::ip::address::from_string(host_);
        boost::asio::ip::tcp::endpoint ep(
            boost::asio::ip::address::from_string(host_), port_);
        socket_.async_connect(ep, [this](const boost::system::error_code &ec) {
            if (has_connected_) {
                return;
            }
            if (ec) {
                has_connected_ = false;
                {
                    std::unique_lock<std::mutex> lock(conn_mtx_);
                    conn_val = true;
                }
                conn_cond_.notify_one();
                return;
            } else {
                has_connected_ = true;
                {
                    std::unique_lock<std::mutex> lock(conn_mtx_);
                    conn_val = true;
                }

                // 一直循环读取
                do_read();

                conn_cond_.notify_one();
                printf("connected!\n");
            }
        });
        // 条件变量起定时作用
        wait_conn(timeout);
        return has_connected_;
    }

    template <typename T> T calcThread(std::uint64_t req_id) {
        std::string curr;
        // 等待条件变量
        std::unique_lock<std::mutex> slock(m_pro_mtx_);
        m_pro_cond_.wait(slock, [this, req_id] {
            return !m_product_.empty() &&
                   m_product_.find(req_id) != m_product_.end();
        });
        curr = m_product_[req_id];
        m_product_.erase(req_id); // 删除该信号
        slock.unlock();

        // 解码
        RPCbufferPack::msgpack_codec codec;
        auto tp = codec.unpack<std::tuple<int, T>>(curr.data(), curr.size());

        // 返回结果
        return std::get<1>(tp);
    }

    // 阻塞式调用
    template <typename T, typename... Args>
    T call(const std::string &rpc_name, Args &&...args) {
        req_mtx_.lock();
        std::uint64_t tmpReqId = m_req_id;
        m_req_id++;
        req_mtx_.unlock();

        // 把发送信息添加到发送队列

        RPCbufferPack::msgpack_codec codec;
        auto que = codec.pack_args(rpc_name, std::forward<Args>(args)...);
        {
            std::unique_lock<std::mutex> lock(write_mtx_);
            write_box_.emplace_back(client_message_type{
                tmpReqId, request_type::req_res,
                std::make_shared<buffer_type>(std::move(que))});
        }

        // write(tmpReqId, request_type::req_res, std::move(que));
        return calcThread<T>(tmpReqId);
    }

    // 非阻塞式future调用,使用get()得到结果
    template <typename T, typename... Args>
    std::shared_ptr<std::future<T>> async_call(const std::string &rpc_name,
                                               Args &&...args) {
        req_mtx_.lock();
        std::uint64_t tmpReqId = m_req_id;
        m_req_id++;
        req_mtx_.unlock();

        // 把发送信息添加到发送队列

        RPCbufferPack::msgpack_codec codec;
        auto que = codec.pack_args(rpc_name, std::forward<Args>(args)...);
        {
            std::unique_lock<std::mutex> lock(write_mtx_);
            write_box_.emplace_back(client_message_type{
                tmpReqId, request_type::req_res,
                std::make_shared<buffer_type>(std::move(que))});
        }

        // 异步线程等待回复
        auto ret = std::make_shared<std::future<T>>(std::async(
            std::launch::async, &rpc_client::calcThread<T>, this, tmpReqId));
        return ret;
    }

  private:
    void stop() {
        if (thd_ != nullptr) {
            ioservice_.stop();
            if (thd_->joinable()) {
                thd_->join();
            }
            thd_ = nullptr;
        }
        // 关闭写线程
        if (write_thd_ != nullptr) {
            if (write_thd_->joinable()) {
                write_thd_->join();
            }
            write_thd_ = nullptr;
        }
    }

    void close() {
        boost::system::error_code ignored_ec;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both,
                         ignored_ec);
        socket_.close(ignored_ec);
        has_connected_ = false;
        // 修改写标志
        {
            std::unique_lock<std::mutex> lock(write_mtx_);
            write_fg_ = true;
        }
    }

    bool wait_conn(size_t timeout) {
        if (has_connected_) {
            return true;
        }
        // 最多等待 timeout 秒，或者直到 conn_val 变为 true。
        // result 变量将告诉你等待的结果，超时false，等待到条件的满足为true。
        std::unique_lock<std::mutex> lock(conn_mtx_);
        bool result = conn_cond_.wait_for(lock, std::chrono::seconds(timeout),
                                          [this] { return conn_val; });
        return result;
    }

    void do_read() {
        // 读取协议头
        boost::asio::async_read(
            socket_, boost::asio::buffer(head_, HEAD_LEN),
            [this](boost::system::error_code ec, std::size_t length) {
                if (!socket_.is_open()) {
                    printf("socket close\n");
                    return;
                }
                if (!ec) {
                    std::uint64_t reqidTmp = 0;
                    request_type reqTypeTmp;
                    uint32_t body_len = 0;
                    memcpy(&body_len, head_, 4);
                    memcpy(&reqidTmp, head_ + 4, 8);
                    memcpy(&reqTypeTmp, head_ + 12, 1);
                    if (body_len > 0 && body_len < MAX_BUF_LEN) {
                        if (body_.size() < body_len) {
                            body_.resize(body_len);
                        }
                        read_body(reqidTmp, reqTypeTmp, body_len);
                        return;
                    }
                    if (body_len == 0) {
                        // LOG
                        printf("body information is illeagl!\n");
                        return;
                    }
                } else {
                    // 出错了断开连接
                    printf("error in read head!\n");
                    close();
                    return;
                }
            });
    }

    // 读取携带的信息
    void read_body(std::uint64_t req_id, request_type req_type,
                   size_t body_len) {
        boost::asio::async_read(
            socket_, boost::asio::buffer(body_.data(), body_len),
            [this, req_id, req_type, body_len](boost::system::error_code ec,
                                               std::size_t length) {
                if (!socket_.is_open()) {
                    printf("socket close\n");
                    return;
                }
                if (!ec) {
                    deal_body(req_id, body_.data(), length);
                    // 递归进行下一次读取
                    do_read();
                } else {
                    printf("error in read body!\n");
                    close();
                    return;
                }
            });
    }

    void deal_body(std::uint64_t req_id, const char *data, std::size_t size) {
        RPCbufferPack::msgpack_codec codec;
        auto p = codec.unpack<std::tuple<int>>(data, size);
        result_code tmp = (result_code)std::get<0>(p);
        if (tmp == result_code::OK) {
            printf("call-response success!\n");
            std::string strFromCharArray(data, size);
            // 生产者
            {
                std::unique_lock<std::mutex> slock(m_pro_mtx_);
                m_product_.emplace(req_id, std::move(strFromCharArray));
            }
            m_pro_cond_.notify_all();

        } else {
            printf("call-response fail!\n");
        }
    }

    void write_callback() {
        while (!write_fg_) {
            while (!write_box_.empty()) {
                auto &msg = write_box_.front();
                uint32_t sendsz = msg.content->size();
                // 以下4个buffer，用的是地址，发送前需要保证内容仍存在
                std::array<boost::asio::const_buffer, 4> write_buffers;
                write_buffers[0] =
                    boost::asio::buffer(&sendsz, sizeof(uint32_t));
                write_buffers[1] =
                    boost::asio::buffer(&msg.req_id, sizeof(uint64_t));
                write_buffers[2] =
                    boost::asio::buffer(&msg.req_type, sizeof(request_type));
                write_buffers[3] =
                    boost::asio::buffer(msg.content->data(), sendsz);
                boost::asio::write(socket_, write_buffers);
                {
                    std::unique_lock<std::mutex> lock(write_mtx_);
                    write_box_.pop_front();
                }
            }
        }
    }

    // 异步发送请求，没有并发保护
    void write(std::uint64_t req_id, request_type type, buffer_type &&message) {
        size_t size = message.size();
        assert(size < MAX_BUF_LEN);
        std::array<boost::asio::const_buffer, 4> write_buffers;
        uint32_t write_size_curr = message.size();
        write_buffers[0] =
            boost::asio::buffer(&write_size_curr, sizeof(uint32_t));
        write_buffers[1] = boost::asio::buffer(&req_id, sizeof(uint64_t));
        write_buffers[2] = boost::asio::buffer(&type, sizeof(request_type));
        write_buffers[3] = boost::asio::buffer(message.data(), write_size_curr);
        boost::asio::async_write(
            socket_, write_buffers,
            [this](boost::system::error_code error, std::size_t length) {
                if (!error) {
                    std::cout << "call completed. Bytes transferred: " << length
                              << std::endl;
                } else {
                    std::cerr << "call error: " << error.message() << std::endl;
                }
            });
    }

  private:
    boost::asio::io_service ioservice_; // 事件分发器
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_service::work work_;
    std::shared_ptr<std::thread> thd_ = nullptr;

    std::string host_;
    unsigned short port_ = 0;
    char head_[HEAD_LEN] = {};
    std::vector<char> body_;

    std::atomic_bool has_connected_ = {false};
    std::mutex conn_mtx_; // 连接定时的条件变量的互斥锁
    std::condition_variable conn_cond_; // 连接定时的条件变量
    bool conn_val = false;

    std::mutex req_mtx_;    // 请求id的并发保护锁
    std::uint64_t m_req_id; // 请求id

    // 收到消息的通知机制，生产者消费者模型
    std::mutex m_pro_mtx_;
    std::condition_variable m_pro_cond_;
    std::unordered_map<std::uint64_t, std::string> m_product_;

    // 发送消息的通知机制
    bool write_fg_ = false;
    std::shared_ptr<std::thread> write_thd_ = nullptr;
    std::mutex write_mtx_;
    struct client_message_type {
        std::uint64_t req_id;
        request_type req_type;
        std::shared_ptr<buffer_type> content;
    };
    std::deque<client_message_type> write_box_;
};

#endif