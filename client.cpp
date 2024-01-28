///*ʾ��1*/
// #include <iostream>
// #include <thread>
// #include "rpc_client.h"
//
// using namespace std;
//
//
//// ��װ�ͻ����߼��ĺ���
// void clientFunction(std::string name, int a, int b) {
//	rpc_client client("127.0.0.1", 9000);// IP ��ַ���˿ں�
//	std::cout << "Address of rpc_client instance: " << &client << std::endl;
//	bool has_connected = client.connect(5);
//	/*û�н����������˳�����*/
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

///*ʾ��2*/
// #include <iostream>
// #include <thread>
// #include "rpc_client.h"
//
// using namespace std;
//
// static rpc_client client("127.0.0.1", 9000);// IP ��ַ���˿ں�
//
//// ��װ�ͻ����߼��ĺ���
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
//	/*�趨��ʱ 5s������Ĭ��Ϊ 3s����connect ��ʱ���� false���ɹ����� true*/
//	bool has_connected = client.connect(5);
//	/*û�н����������˳�����*/
//	if (!has_connected) {
//		std::cout << "connect timeout" << std::endl;
//		exit(-1);
//	}
//
//	// ����6���߳�
//	std::thread threads[6];
//
//	// ʹ��clientFunction����ÿ���߳�
//	for (int i = 0; i < 6; ++i) {
//		string tmp = "num calc:";
//		tmp += to_string(i + 2);
//		tmp += " + ";
//		tmp += to_string(i * 9);
//		tmp += " = ";
//		threads[i] = std::thread(clientFunction, tmp, i + 2, i * 9);
//	}
//
//	// �ȴ������߳����
//	for (int i = 0; i < 6; ++i) {
//		threads[i].join();
//	}
//
//
//	return 0;
// }

/*ʾ��3*/
#include <iostream>
#include <thread>
#include "rpc_client.h"

using namespace std;

// ��װ�ͻ����߼��ĺ���
void clientFunction(std::string name, int a, int b) {
    Sleep((a - 2) * 1000);
    rpc_client client("127.0.0.1", 9000); // IP ��ַ���˿ں�
    std::cout << "Address of rpc_client instance: " << &client << std::endl;
    bool has_connected = client.connect(5);
    /*û�н����������˳�����*/
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
    // ����6���߳�
    std::thread threads[6];

    // ʹ��clientFunction����ÿ���߳�
    for (int i = 0; i < 6; ++i) {
        string tmp = "num calc:";
        tmp += to_string(i + 2);
        tmp += " + ";
        tmp += to_string(i * 9);
        tmp += " = ";
        threads[i] = std::thread(clientFunction, tmp, i + 2, i * 9);
    }

    // �ȴ������߳����
    for (int i = 0; i < 6; ++i) {
        threads[i].join();
    }

    return 0;
}
