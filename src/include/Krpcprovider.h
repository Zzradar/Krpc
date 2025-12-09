#ifndef _Krpcprovider_H__
#define _Krpcprovider_H__
#include "google/protobuf/service.h"
#include "zookeeperutil.h"
#include<muduo/net/TcpServer.h>
#include<muduo/net/EventLoop.h>
#include<muduo/net/InetAddress.h>
#include<muduo/net/TcpConnection.h>
#include<google/protobuf/descriptor.h>
#include<functional>
#include<string>
#include<unordered_map>
#include<memory>
#include<cstdint>

#include "Krpcprotocol.h"

class KrpcProvider
{
public:
    //这里是提供给外部使用的，可以发布rpc方法的函数接口。
    void NotifyService(google::protobuf::Service* service);
      ~KrpcProvider();
    //启动rpc服务节点，开始提供rpc远程网络调用服务
    void Run();
private:
    muduo::net::EventLoop event_loop;
    struct ServiceInfo
    {
        std::unique_ptr<google::protobuf::Service> service;
        std::unordered_map<std::string, const google::protobuf::MethodDescriptor*> method_map;
    };
    std::unordered_map<std::string, ServiceInfo>service_map;//保存服务对象和rpc方法
    
    void OnConnection(const muduo::net::TcpConnectionPtr& conn);
    void OnMessage(const muduo::net::TcpConnectionPtr& conn, muduo::net::Buffer* buffer, muduo::Timestamp receive_time);
    void SendRpcResponse(const muduo::net::TcpConnectionPtr& conn, uint64_t request_id, google::protobuf::Message* response);

    class SendResponseClosure : public google::protobuf::Closure {
    public:
        SendResponseClosure(KrpcProvider *provider,
                            muduo::net::TcpConnectionPtr conn,
                            uint64_t request_id,
                            google::protobuf::Message *response);
        void Run() override;

    private:
        KrpcProvider *provider_;
        muduo::net::TcpConnectionPtr conn_;
        uint64_t request_id_;
        google::protobuf::Message *response_;
    };
};
#endif 
