# workflow-k8s
本项目旨在将Workflow的服务治理与kubernetes的自动部署相融合，打造稳定、便捷的服务体系。

Kubernetes API Server提供了HTTP(S)接口，当集群内Pod发生变动后，会及时将这些事件推送给监听者，本项目依托Workflow的服务治理体系，使Pod变动事件及时生效，保障服务持续稳定运行。通过本项目提供的`WeightedRandomHandler`、`ConsistentHashHandler`、`ManualSelectHandler`等事件处理器，用户可快速将已经部署到k8s的服务管理起来，这些pod可以在不同的应用里、在不同的命名空间下、甚至可以在不同的集群中。用户也可以通过自己实现Handler，实现让自己满意的服务治理体系，例如不同性能的GPU可以配置不同的负载、同一种类型的请求发往同一个集群等等。

服务治理只是本项目的主要目标，本项目实现的是kubernetes pod事件监听机制，用户可以通过自行编写Handler，监控某应用下的pod发生的事件，当用户关心的事件发生时执行某些操作等。

## 快速开始

```cpp
#include "workflow/WFTaskFactory.h"
#include "workflow/WFFacilities.h"
#include "k8s-service/K8sServiceManager.h"
#include "k8s-service/K8sServiceHandler.h"

WFFacilities::WaitGroup wg(1);

int main() {
    k8s::WatchConfig wcfg {
        .url = "https://your.k8s.api.server:6443",
        .name_space = "your-namespace",
        .label_selector = "your-label in (what-you-want-to-watch)",
        .cert = "your.cert",
        .key = "your.key"
    };

    // 创建一个用于处理消息的handler
    k8s::WeightedRandomHandler hdl("what.you.like.host", true, &ADDRESS_PARAMS_DEFAULT);
    k8s::ServiceManager mng;
    mng.start_watch_pods(wcfg, &hdl); // 开始观察该应用pod的变动情况
    hdl.wait_first(); // 等待任意一个pod就绪

    const char *url = "http://what.you.like.host:{your-service-port}/your-path";
    auto task = WFTaskFactory::create_http_task(url, 2, 2, [](WFHttpTask *) {
        wg.done();
    });

    task->start();
    wg.wait();

    mng.shutdown(); // 停止观察
    mng.wait_finish(); // 等待所有异步任务退出

    return 0;
}
```
