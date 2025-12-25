#include "zookeeperutil.h"
#include "Krpcapplication.h"
#include <mutex>
#include "KrpcLogger.h"
#include <condition_variable>

// 构造函数，初始化ZooKeeper客户端句柄为空
ZkClient::ZkClient() : m_zhandle(nullptr), m_connected(false) {}

// 析构函数，关闭ZooKeeper连接
ZkClient::~ZkClient() {
    if (m_zhandle != nullptr) {
        zookeeper_close(m_zhandle);  // 关闭ZooKeeper连接
    }
}

// 启动ZooKeeper客户端，连接ZooKeeper服务器
void ZkClient::Start() {
    // 从配置文件中读取ZooKeeper服务器的IP和端口
    std::string host = KrpcApplication::GetInstance().GetConfig().Load("zookeeperip");
    std::string port = KrpcApplication::GetInstance().GetConfig().Load("zookeeperport");
    std::string connstr = host + ":" + port;  // 拼接连接字符串
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connected = false;
    }

    /*
    zookeeper_mt：多线程版本
    ZooKeeper的API客户端程序提供了三个线程：
    1. API调用线程
    2. 网络I/O线程（使用pthread_create和poll）
    3. watcher回调线程（使用pthread_create）
    */

    // 使用zookeeper_init初始化一个ZooKeeper客户端对象，异步建立与服务器的连接
    m_zhandle = zookeeper_init(connstr.c_str(), ZkClient::GlobalWatcher, 6000, nullptr, this, 0);
    if (nullptr == m_zhandle) {  // 初始化失败
        LOG(ERROR) << "zookeeper_init error";
        exit(EXIT_FAILURE);  // 退出程序
    }

    // 等待连接成功
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait(lock, [this] { return m_connected; });  // 阻塞等待，直到连接成功
    LOG(INFO) << "zookeeper_init success";  // 记录日志，表示连接成功
}

// 创建ZooKeeper节点（若存在且类型不符则删除后重建；类型一致则可更新数据）
void ZkClient::Create(const char *path, const char *data, int datalen, int state) {
    char path_buffer[256] = {0};
    int bufferlen = sizeof(path_buffer);

    struct Stat stat {};
    const int exists_flag = zoo_exists(m_zhandle, path, 0, &stat);
    if (exists_flag == ZNONODE) {
        int create_flag = zoo_create(m_zhandle, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE, state, path_buffer, bufferlen);
        if (create_flag == ZOK) {
            LOG(INFO) << "znode create success... path:" << path;
            return;
        }
        LOG(ERROR) << "znode create failed... path:" << path << ", err=" << create_flag;
        exit(EXIT_FAILURE);
    }

    if (exists_flag == ZOK) {
        const bool is_ephemeral = (stat.ephemeralOwner != 0);
        const bool want_ephemeral = (state == ZOO_EPHEMERAL);
        if (is_ephemeral != want_ephemeral) {
            int delete_flag = zoo_delete(m_zhandle, path, -1);
            if (delete_flag != ZOK && delete_flag != ZNONODE) {
                LOG(ERROR) << "znode delete failed before recreate... path:" << path << ", err=" << delete_flag;
                exit(EXIT_FAILURE);
            }
            int create_flag = zoo_create(m_zhandle, path, data, datalen, &ZOO_OPEN_ACL_UNSAFE, state, path_buffer, bufferlen);
            if (create_flag == ZOK) {
                LOG(INFO) << "znode recreate success... path:" << path;
                return;
            }
            LOG(ERROR) << "znode recreate failed... path:" << path << ", err=" << create_flag;
            exit(EXIT_FAILURE);
        } else {
            // 类型相同，如有数据则更新
            if (data != nullptr && datalen > 0) {
                const int set_flag = zoo_set(m_zhandle, path, data, datalen, -1);
                if (set_flag != ZOK) {
                    LOG(ERROR) << "znode set data failed... path:" << path << ", err=" << set_flag;
                    exit(EXIT_FAILURE);
                }
            }
            return;
        }
    }

    LOG(ERROR) << "znode check failed... path:" << path << ", err=" << exists_flag;
    exit(EXIT_FAILURE);
}

// 获取ZooKeeper节点的数据
std::string ZkClient::GetData(const char *path) {
    char buf[64];  // 用于存储节点数据
    int bufferlen = sizeof(buf);

    // 获取指定节点的数据
    int flag = zoo_get(m_zhandle, path, 0, buf, &bufferlen, nullptr);
    if (flag != ZOK) {  // 获取失败
        LOG(ERROR) << "zoo_get error";
        return "";  // 返回空字符串
    } else {  // 获取成功
        return buf;  // 返回节点数据
    }
    return "";  // 默认返回空字符串
}

void ZkClient::GlobalWatcher(zhandle_t *zh, int type, int status, const char *path, void *watcherCtx) {
    if (type == ZOO_SESSION_EVENT && status == ZOO_CONNECTED_STATE && watcherCtx != nullptr) {
        auto *client = static_cast<ZkClient *>(watcherCtx);
        client->NotifyConnected();
    }
}

void ZkClient::NotifyConnected() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connected = true;
    }
    m_cv.notify_all();
}

std::vector<std::string> ZkClient::GetChildren(const char *path) {
    struct String_vector children;
    std::vector<std::string> result;
    int flag = zoo_get_children(m_zhandle, path, 0, &children);
    if (flag != ZOK) {
        LOG(ERROR) << "zoo_get_children error";
        return result;
    }
    for (int i = 0; i < children.count; ++i) {
        result.emplace_back(children.data[i]);
    }
    deallocate_String_vector(&children);
    return result;
}
