#ifndef K8S_SERVICE_HANDLER_H
#define K8S_SERVICE_HANDLER_H

#include <string>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <condition_variable>
#include "workflow/UpstreamManager.h"

#include "K8sGlobal.h"

namespace k8s {

class BasicServiceHandler : public EventHandlerBase {
public:
    BasicServiceHandler() = default;
    virtual ~BasicServiceHandler() = default;

    void on_start_watch(WatchConfig *cfg) override;
    void on_stop_watch(const WatchConfig *cfg) override;
    bool on_pod(const WatchConfig *cfg, const Pod &pod) override;
    bool on_reset(const WatchConfig *cfg) override;
    bool on_error(WatchConfig *, const WatchError &) override {
        return true;
    }

    void wait_first();
    size_t get_alive_cnt() const { return alive_cnt; }
    size_t get_total_cnt() const { return total_cnt; }

protected:
    // TODO remove const Pod & ?
    virtual void on_add_server(const WatchConfig *, const Pod &, const std::string &address) { }
    virtual void on_remove_server(const WatchConfig *, const Pod &, const std::string &address) { }

protected:
    struct PodState {
        std::string ip;
        bool alive;
    };
    using pod_states_t = std::unordered_map<std::string, PodState>; // pod_name -> pod_state
    std::unordered_map<const WatchConfig *, pod_states_t> states;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<size_t> alive_cnt{0};
    std::atomic<size_t> total_cnt{0};
};

class UpstreamServiceHandler : public BasicServiceHandler {
public:
    UpstreamServiceHandler(const std::string &name, const AddressParams *params)
        : name(name), address_params(*params) { }
    virtual ~UpstreamServiceHandler() = default;

protected:
    // on_{add, remove}_server are under BasicServiceHandler::mtx, be careful
    void on_add_server(const WatchConfig *cfg, const Pod &, const std::string &address) override {
        if(cfg->port == 0)
            UpstreamManager::upstream_add_server(name, address, &address_params);
        else
            UpstreamManager::upstream_add_server(name, address + ":" + std::to_string(cfg->port), &address_params);
    }

    void on_remove_server(const WatchConfig *cfg, const Pod &, const std::string &address) override {
        if(cfg->port == 0)
            UpstreamManager::upstream_remove_server(name, address);
        else
            UpstreamManager::upstream_remove_server(name, address + ":" + std::to_string(cfg->port));
    }

protected:
    std::string name;             // server's upstream name
    AddressParams address_params; // default address params
};

class WeightedRandomHandler : public UpstreamServiceHandler {
public:
    WeightedRandomHandler(const std::string &name, bool try_another,
        const AddressParams *params)
        : UpstreamServiceHandler(name, params)
    {
        UpstreamManager::upstream_create_weighted_random(name, try_another);
    }
    virtual ~WeightedRandomHandler() {
        UpstreamManager::upstream_delete(name);
    }
};

class ConsistentHashHandler : public UpstreamServiceHandler {
public:
    ConsistentHashHandler(const std::string &name,
        upstream_route_t consistent_hash, const AddressParams *params)
        : UpstreamServiceHandler(name, params)
    {
        UpstreamManager::upstream_create_consistent_hash(name, consistent_hash);
    }
    virtual ~ConsistentHashHandler() {
        UpstreamManager::upstream_delete(name);
    }
};

class ManualSelectHandler : public UpstreamServiceHandler {
public:
    ManualSelectHandler(const std::string &name, upstream_route_t select,
        bool try_another, upstream_route_t consistent_hash,
        const AddressParams *params)
        : UpstreamServiceHandler(name, params)
    {
        UpstreamManager::upstream_create_manual(name, select, try_another, consistent_hash);
    }
    virtual ~ManualSelectHandler() {
        UpstreamManager::upstream_delete(name);
    }
};

} // namespace k8s

#endif // K8S_SERVICE_HANDLER_H
