#include "Krpccontroller.h"

// 构造函数，初始化控制器状态
Krpccontroller::Krpccontroller() : m_failed(false), m_errText(""), m_timeout_ms(0) {}

// 重置控制器状态，将失败标志和错误信息清空
void Krpccontroller::Reset() {
    m_failed = false;  // 重置失败标志
    m_errText = "";    // 清空错误信息
    m_timeout_ms = 0;   // 重置自定义超时
}

// 判断当前RPC调用是否失败
bool Krpccontroller::Failed() const {
    return m_failed;  // 返回失败标志
}

// 获取错误信息
std::string Krpccontroller::ErrorText() const {
    return m_errText;  // 返回错误信息
}

// 设置RPC调用失败，并记录失败原因
void Krpccontroller::SetFailed(const std::string &reason) {
    m_failed = true;   // 设置失败标志
    m_errText = reason; // 记录失败原因
}

// 设置超时时间，单位毫秒
void Krpccontroller::SetTimeoutMs(int timeout_ms) {
    m_timeout_ms = timeout_ms;
}

// 获取当前配置的超时时间
int Krpccontroller::TimeoutMs() const {
    return m_timeout_ms;
}

// 以下功能未实现，是RPC服务端提供的取消功能
// 开始取消RPC调用（未实现）
void Krpccontroller::StartCancel() {
    // 目前为空，未实现具体功能
}

// 判断RPC调用是否被取消（未实现）
bool Krpccontroller::IsCanceled() const {
    return false;  // 默认返回false，表示未被取消
}

// 注册取消回调函数（未实现）
void Krpccontroller::NotifyOnCancel(google::protobuf::Closure* callback) {
    // 目前为空，未实现具体功能
}