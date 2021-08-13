#ifndef K8S_GLOBAL_H
#define K8S_GLOBAL_H

#include <string>
#include <vector>

namespace k8s {

// default for k8s api.v1
inline namespace v1 {

struct ObjectMeta {
    std::string name;
    std::string generate_name;
    std::string name_space;
    std::string self_link;
    std::string uid;
    std::string resource_version;
    std::string creation_timestamp;
    std::string deletion_timestamp;
    std::vector<std::pair<std::string, std::string>> labels;
};

struct ContainerStatus {
    bool ready{false};
    int restart_count{0};
    std::string name;
    std::string state; // TODO running, waiting, terminated started_at
    std::string image;
    std::string image_id;
    std::string container_id;
};

struct PodCondition {
    std::string type;
    std::string status;
    std::string reason;
    std::string message;
    std::string last_probe_time;
    std::string last_transition_time;
};

struct PodStatus {
    std::string phase;
    std::string host_ip;
    std::string pod_ip;
    std::string start_time;
    std::string reason;
    std::vector<std::string> pod_ips;
    std::vector<PodCondition> conditions;
    std::vector<ContainerStatus> container_statuses;
};

struct ErrorStatus {
    int code{0};
    std::string status;
    std::string message;
    std::string reason;
};

struct Pod {
    std::string type;
    ObjectMeta metadata;
    PodStatus status;
    ErrorStatus error_status;
};

struct PodList {
    std::string selfLink;
    std::string resource_version;
    std::vector<Pod> items;
};

} // namespace v1

struct WatchConfig {
    std::string url;
    std::string name_space;
    std::string label_selector;
    std::string cert;
    std::string key;
    int port;
    void *user_data;
};

enum {
    ERR_UNDEFINED = 0,
    ERR_SUCCESS,
    ERR_INNER, // we want to retry
    ERR_TASK,
    ERR_HTTP,
    ERR_SSL,
    ERR_JSON
};

struct WatchError {
    int type;
    union {
        struct {
            int state;
            int error;
        } task;
        struct {
            int http_code;
        } http;
        struct {
            const char *error;
        } ssl;
        struct {
            const char *first;
            const char *last;
        } json;
    };
};

inline std::string get_error_string(const WatchError &e) {
    switch(e.type) {
    case ERR_SUCCESS:
        return "No Error";
    case ERR_TASK:
        return "Task Error";
    case ERR_HTTP:
        return "Http Error";
    case ERR_SSL:
        return "SSL Error";
    case ERR_JSON:
        return "Json Error";
    default:
        return "Unknown Error";
    }
}

struct EventHandlerBase {
    EventHandlerBase() = default;
    virtual ~EventHandlerBase() = default;

    virtual void on_start_watch(WatchConfig *cfg) {}
    virtual void on_stop_watch(const WatchConfig *cfg) {}
    virtual bool on_pod(const WatchConfig *cfg, const Pod &) { return true; }
    virtual bool on_reset(const WatchConfig *cfg) { return true; }
    virtual bool on_error(WatchConfig *cfg, const WatchError &) { return false; }
};

struct HandlerProtector {
    HandlerProtector() = default;
    virtual ~HandlerProtector() = default;
    virtual EventHandlerBase *acquire_handler(const WatchConfig *) { return nullptr; }
    virtual void release_handler(EventHandlerBase*) {}
};

struct WatchContext {
    bool user_exit{false};
    bool need_reset{false};
    int current_status{ERR_SUCCESS};
    int last_status{ERR_UNDEFINED};
    size_t total_cnt{0};
    size_t retry_cnt{0};

    WatchError error;
    std::string ssl_errmsg;
    std::string resource_version;
    WatchConfig *config{nullptr};
    HandlerProtector *protector{nullptr};
};

} // namespace k8s

#endif // K8S_GLOBAL_H
