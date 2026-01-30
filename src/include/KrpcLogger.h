#ifndef KRPC_LOG_H
#define KRPC_LOG_H
#include<glog/logging.h>
#include<cstdlib>
#include<mutex>
#include<string>
//采用RAII的思想
class KrpcLogger
{
public:
      //构造函数，自动初始化glog
      explicit KrpcLogger(const char *argv0)
      {
        InitOnce(argv0, 1, true, true);
      }
      ~KrpcLogger() = default;

      // 初始化（只执行一次）
      static void InitOnce(const char *argv0, int minloglevel, bool logtostderr, bool colorlogtostderr)
      {
        static std::once_flag init_flag;
        std::call_once(init_flag, [argv0, minloglevel, logtostderr, colorlogtostderr]() {
          google::InitGoogleLogging(argv0);
          FLAGS_colorlogtostderr = colorlogtostderr;
          FLAGS_logtostderr = logtostderr;
          FLAGS_minloglevel = minloglevel;
          std::atexit(google::ShutdownGoogleLogging);
        });
      }
      //提供静态日志方法
      static void Info(const std::string &message)
      {
        LOG(INFO)<<message;
      }
      static void Warning(const std::string &message){
        LOG(WARNING)<<message;
      }
      static void ERROR(const std::string &message){
        LOG(ERROR)<<message;
      }
          static void Fatal(const std::string& message) {
        LOG(FATAL) << message;
    }
//禁用拷贝构造函数和重载赋值函数
private:
    KrpcLogger(const KrpcLogger&)=delete;
    KrpcLogger& operator=(const KrpcLogger&)=delete;
};

#endif
