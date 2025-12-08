#ifndef _zookeeperutil_h_
#define _zookeeperutil_h_

#include <semaphore.h>
#include <zookeeper/zookeeper.h>
#include <string>
#include <condition_variable>
#include <mutex>

//封装的zk客户端
class ZkClient
{
public:
    ZkClient();
    ~ZkClient();
    //zkclient启动连接zkserver
    void Start();
    //在zkserver中创建一个节点，根据指定的path
    void Create(const char* path,const char* data,int datalen,int state=0);
    //根据参数指定的znode节点路径，或者znode节点值
    std::string GetData(const char* path);
private:
    //Zk的客户端句柄
    zhandle_t* m_zhandle;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_connected;

    static void GlobalWatcher(zhandle_t *zh, int type, int status, const char *path, void *watcherCtx);
    void NotifyConnected();
};
#endif
