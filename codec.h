#pragma once
#ifndef TINY_RPC_CODEC_H_
#define TINY_RPC_CODEC_H_

#include <msgpack.hpp>

using buffer_type = msgpack::sbuffer;

namespace RPCbufferPack {

struct msgpack_codec {
  public:
    const static size_t init_size = 2 * 1024; //  MessagePack 缓冲区的初始大小

    // 打包参数
    template <typename... Args> static buffer_type pack_args(Args &&...args) {
        buffer_type buffer(init_size);
        // 使用完美转发，保留参数的引用类型，并组成一个tuple，方便序列化
        msgpack::pack(buffer,
                      std::forward_as_tuple(std::forward<Args>(args)...));
        return buffer;
    }

    // 打包枚举类型，并返回std::string 对象。
    template <
        typename Arg, typename... Args,
        typename = typename std::enable_if<std::is_enum<Arg>::value>::type>
    static std::string pack_args_str(Arg arg, Args &&...args) {
        buffer_type buffer(init_size);
        msgpack::pack(buffer, std::forward_as_tuple(
                                  (int)arg, std::forward<Args>(args)...));
        return std::string(buffer.data(), buffer.size());
    }

    // 打包单个参数
    template <typename T> buffer_type pack(T &&t) const {
        buffer_type buffer;
        msgpack::pack(buffer, std::forward<T>(t));
        return buffer;
    }

    // 解包，返回解包后的对象
    template <typename T> T unpack(char const *data, size_t length) {
        try {
            msgpack::unpack(msg_, data, length);
            return msg_.get().as<T>();
        } catch (...) {
            throw std::invalid_argument("unpack failed: Args not match!");
        }
    }

  private:
    msgpack::unpacked msg_;
};

} // namespace RPCbufferPack

#endif