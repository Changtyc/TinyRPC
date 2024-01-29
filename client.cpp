///*示例1*/
// #include <iostream>
// #include <thread>
// #include "rpc_client.h"
//
// using namespace std;
//
//
//// 封装客户端逻辑的函数
// void clientFunction(std::string name, int a, int b) {
//	rpc_client client("127.0.0.1", 9000);// IP 地址，端口号
//	std::cout << "Address of rpc_client instance: " << &client << std::endl;
//	bool has_connected = client.connect(5);
//	/*没有建立连接则退出程序*/
//	if (!has_connected) {
//		std::cout << "connect timeout" << std::endl;
//		return;
//	}
//
//	auto asy_ = client.async_call<std::string>("GreetFun",
//"async_call_hello_tycrpc");
//
//	std::string ret = client.call<std::string>("GreetFun", name);
//	int ans = client.call<int>("calcFun", a, b);
//
//	cout << "GreetFun = :" << ret << endl;
//	cout << "calcFun = :" << ans << endl;
//
//	std::string aaaa = asy_->get();
//	cout << "GreetFun_async = :" << aaaa << endl;
//
//	printf("===========================\n");
// }
//
//
//
// int main() {
//	clientFunction("tycRPC", 100, 55);
//	return 0;
// }

///*示例2*/
// #include <iostream>
// #include <thread>
// #include "rpc_client.h"
//
// using namespace std;
//
// static rpc_client client("127.0.0.1", 9000);// IP 地址，端口号
//
//// 封装客户端逻辑的函数
// void clientFunction(std::string name, int a, int b) {
//	std::string ret = client.call<std::string>("GreetFun", name);
//	int ans = client.call<int>("calcFun", a, b);
//
//	cout << "GreetFun = :" << ret << endl;
//	cout << "calcFun = :" << ans << endl;
// }
//
//
// int main() {
//	/*设定超时 5s（不填默认为 3s），connect 超时返回 false，成功返回 true*/
//	bool has_connected = client.connect(5);
//	/*没有建立连接则退出程序*/
//	if (!has_connected) {
//		std::cout << "connect timeout" << std::endl;
//		exit(-1);
//	}
//
//	// 创建6个线程
//	std::thread threads[6];
//
//	// 使用clientFunction启动每个线程
//	for (int i = 0; i < 6; ++i) {
//		string tmp = "num calc:";
//		tmp += to_string(i + 2);
//		tmp += " + ";
//		tmp += to_string(i * 9);
//		tmp += " = ";
//		threads[i] = std::thread(clientFunction, tmp, i + 2, i * 9);
//	}
//
//	// 等待所有线程完成
//	for (int i = 0; i < 6; ++i) {
//		threads[i].join();
//	}
//
//
//	return 0;
// }

/*示例3*/
#include <iostream>
#include <thread>
#include "rpc_client.h"

using namespace std;

// 封装客户端逻辑的函数
void clientFunction(std::string name, int a, int b) {
    Sleep((a - 2) * 1000);
    rpc_client client("127.0.0.1", 9000); // IP 地址，端口号
    std::cout << "Address of rpc_client instance: " << &client << std::endl;
    bool has_connected = client.connect(5);
    /*没有建立连接则退出程序*/
    if (!has_connected) {
        std::cout << "connect timeout" << std::endl;
        return;
    }
    std::string ret = client.call<std::string>("GreetFun", name);
    int ans = client.call<int>("calcFun", a, b);

    cout << "GreetFun = :" << ret << endl;
    cout << "calcFun = :" << ans << endl;
    printf("===========================\n");
}

int main() {
    // 创建6个线程
    std::thread threads[6];

    // 使用clientFunction启动每个线程
    for (int i = 0; i < 6; ++i) {
        string tmp = "num calc:";
        tmp += to_string(i + 2);
        tmp += " + ";
        tmp += to_string(i * 9);
        tmp += " = ";
        threads[i] = std::thread(clientFunction, tmp, i + 2, i * 9);
    }

    // 等待所有线程完成
    for (int i = 0; i < 6; ++i) {
        threads[i].join();
    }

    return 0;
}
