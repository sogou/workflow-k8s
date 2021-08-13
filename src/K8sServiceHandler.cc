#include <cassert>
#include "K8sServiceHandler.h"

using namespace std;

namespace k8s {

void BasicServiceHandler::on_start_watch(WatchConfig *cfg) {
    std::lock_guard<std::mutex> lg(mtx);
    states.emplace(cfg, pod_states_t{});
}

void BasicServiceHandler::on_stop_watch(const WatchConfig *cfg) {
    std::lock_guard<std::mutex> lg(mtx);
    auto it = states.find(cfg);
    assert(it != states.end());

    Pod pod;
    pod.type = "STOPPED";
    for(auto &kv : it->second) {
        auto &pod_state = kv.second;
        if(pod_state.alive == true) {
            alive_cnt--;
            pod_state.alive = false;
            pod.metadata.name = kv.first;
            on_remove_server(cfg, pod, pod_state.ip);
        }
    }

    total_cnt -= it->second.size();
    states.erase(it);
}

bool BasicServiceHandler::on_pod(const WatchConfig *cfg, const Pod &pod) {
    const std::string &pod_name = pod.metadata.name;
    const std::string &pod_ip = pod.status.pod_ip;
    bool all_ok = true;
    bool pod_ready = false;

    // maybe pod.type is ERROR or BOOKMARK or something else
    if(pod_name.empty())
        return true;

    for(const auto &c : pod.status.conditions) {
        if(c.status != "True")
            all_ok = false;
        if(c.type == "Ready" && c.status == "True")
            pod_ready = true;
    }

    std::lock_guard<std::mutex> lg(mtx);
    pod_states_t &state = states[cfg];

    auto it = state.find(pod_name);
    if(it == state.end()) {
        it = state.emplace(pod_name, PodState {pod_ip, false}).first;
        total_cnt++;
    }

    PodState &ps = it->second;
    if(ps.ip != pod_ip)
        ps.ip = pod_ip;

    if(pod.type == "ADDED" || pod.type == "MODIFIED") {
        if(all_ok && pod_ready) {
            if(ps.alive == false) {
                ps.alive = true;
                alive_cnt++;
                on_add_server(cfg, pod, pod_ip);
                if(alive_cnt == 1)
                    cv.notify_all();
            }
        }
        else {
            if(ps.alive == true) {
                ps.alive = false;
                alive_cnt--;
                on_remove_server(cfg, pod, pod_ip);
            }
        }
    }
    else if(pod.type == "DELETED") {
        total_cnt--;
        state.erase(it);
        if(ps.alive == true) {
            ps.alive = false;
            alive_cnt--;
            on_remove_server(cfg, pod, pod_ip);
        }
    }

    return true;
}

bool BasicServiceHandler::on_reset(const WatchConfig *cfg) {
    std::lock_guard<std::mutex> lg(mtx);
    auto it = states.find(cfg);
    assert(it != states.end());

    Pod pod;
    pod.type = "RESET";
    for(auto &kv : it->second) {
        auto &pod_state = kv.second;
        if(pod_state.alive == true) {
            alive_cnt--;
            pod_state.alive = false;
            pod.metadata.name = kv.first;
            on_remove_server(cfg, pod, pod_state.ip);
        }
    }

    total_cnt -= it->second.size();
    it->second.clear();
    return false;
}

void BasicServiceHandler::wait_first() {
    std::unique_lock<std::mutex> lk(mtx);
    cv.wait(lk, [this]() { return get_alive_cnt() > 0; });
}

} // namespace k8s
