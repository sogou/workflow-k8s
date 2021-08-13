#ifndef K8S_SERVICE_MANAGER_H
#define K8S_SERVICE_MANAGER_H

#include <string>
#include <memory>
#include <vector>
#include "K8sGlobal.h"

namespace k8s {

class SharedServiceManager;

class ServiceManager {
public:
    ServiceManager();
    ServiceManager(ServiceManager&&) = delete;
    ServiceManager& operator=(ServiceManager&&) = delete;
    ~ServiceManager();

    bool start_watch_pods(const WatchConfig &cfg, EventHandlerBase *hdl);
    void shutdown();
    void wait_finish();
    bool wait_finish_for(double sec);
private:
    std::shared_ptr<SharedServiceManager> m;
};

} // namespace k8s

#endif // K8_SERVICE_MANAGER_H
