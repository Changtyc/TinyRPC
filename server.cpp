#include <iostream>
#include "rpc_server.h"
using namespace std;

std::string hello(std::string name) {
    /*可以为 void 返回类型，代表调用后不给远程客户端返回消息*/
    return ("Hello " + name); /*返回给远程客户端的内容*/
}

int calcAB(int a, int b) { return a + b; }

int main() {
    rpc_server server(9000, 6);
    server.register_handler("GreetFun", hello);
    server.register_handler("calcFun", calcAB);
    server.run();
    return 0;
}
