#include <mutex>
#include <chrono>
#include <condition_variable>
#include "workflow/Workflow.h"
#include "workflow/StringUtil.h"

#include "K8sServiceManager.h"
#include "K8sHttpTask.h"

using namespace std;
using namespace protocol;

namespace k8s {

static K8sHttpTask *create_k8s_http_task(WatchContext *ctx);

static void http_callback(K8sHttpTask *task) {
    SeriesWork *series = series_of(task);
    WatchContext *wctx = (WatchContext *)series->get_context();
    WatchConfig *cfg = wctx->config;
    int state = task->get_state();
    int error = task->get_error();
    bool user_ret = true;

    auto deleter = [wctx](EventHandlerBase *hdl) {
        wctx->protector->release_handler(hdl);
    };
    unique_ptr<EventHandlerBase, decltype(deleter)> hdl(
        wctx->protector->acquire_handler(cfg), deleter);

    if(!hdl)
        return;

    if(wctx->user_exit)
        return;

    if(task->get_state() == WFT_STATE_ABORTED)
        return;

    switch (wctx->current_status) {
    case ERR_UNDEFINED:
    case ERR_JSON: // have been processed in K8sHttpResponse::append
    case ERR_INNER:
        break;
    case ERR_SUCCESS:
        if(state != WFT_STATE_SUCCESS) {
            wctx->current_status = ERR_TASK;
            user_ret = hdl->on_error(cfg, WatchError {
                .type = ERR_TASK,
                { .task = {state, error} }
            });
        }
        break;
    default:
        user_ret = hdl->on_error(cfg, wctx->error);
    }

    if(user_ret == false)
        return;

    if(wctx->current_status != ERR_SUCCESS) {
        if(wctx->current_status == wctx->last_status)
            wctx->retry_cnt++;
        else
            wctx->last_status = wctx->current_status;
        wctx->current_status = ERR_SUCCESS;
    }
    else
        wctx->retry_cnt = 0;

    if(wctx->last_status != ERR_SUCCESS) {
        unsigned int delay = min(wctx->retry_cnt * 100, size_t(2000)) * 1000;
        series->push_back(WFTaskFactory::create_timer_task(delay, [](WFTimerTask *task) {
            if(task->get_state() == WFT_STATE_ABORTED)
                series_of(task)->cancel();
        }));
    }

    series->push_back(create_k8s_http_task(wctx));
}

static string create_url_from_context(WatchContext *ctx, bool watch) {
    auto *cfg = ctx->config;
    string url = cfg->url;

    while(!url.empty() && url.back() == '/')
        url.pop_back();

    url.append("/api/v1");
    if(!cfg->name_space.empty())
        url.append("/namespaces/").append(cfg->name_space);
    url.append("/pods?pretty=false");

    if(watch) {
        // server may send bookmark when timeout
        url.append("&watch=true&allowWatchBookmarks=true").
            append("&timeoutSeconds=5");
    }

    if(!ctx->resource_version.empty()) {
        url.append("&resourceVersion=").
            append(ctx->resource_version);
    }

    if(!cfg->label_selector.empty()) {
        url.append("&labelSelector=").
            append(StringUtil::url_encode_component(cfg->label_selector));
    }

    return url;
}

static K8sHttpTask *create_k8s_http_task(WatchContext *ctx) {
    auto *task = new ComplexK8sHttpTask(0, http_callback);
    bool is_watch = !ctx->resource_version.empty();
    auto url = create_url_from_context(ctx, is_watch);
    ParsedURI uri;

    task->set_watch_context(ctx);
    task->get_resp()->set_watch_context(ctx);
    task->get_resp()->set_watch_response(is_watch);

    // init after set context
    URIParser::parse(url, uri);
    task->init(std::move(uri));
    return task;
}

class SharedServiceManager
    : public std::enable_shared_from_this<SharedServiceManager>,
      public HandlerProtector {
public:
    SharedServiceManager() = default;
    virtual ~SharedServiceManager() = default;

    virtual EventHandlerBase *acquire_handler(const WatchConfig *cfg) override;
    virtual void release_handler(EventHandlerBase *) override;

    bool start_watch_pods(const WatchConfig&, EventHandlerBase*);

    void shutdown();
    void wait_finish();
    bool wait_finish_for(double sec);
    void series_done(const SeriesWork *);

private:
    std::map<const WatchConfig*, EventHandlerBase*> cfg_hdl;
    std::mutex mtx;
    condition_variable cv;
    size_t handler_ref{0};
    size_t series_ref{0};
    bool running{true};
};

EventHandlerBase *SharedServiceManager::acquire_handler(const WatchConfig *cfg) {
    std::lock_guard<std::mutex> lg(mtx);
    if(!running)
        return nullptr;

    auto it = cfg_hdl.find(cfg);
    if(it == cfg_hdl.end())
        return nullptr;

    handler_ref++;
    return it->second;
}

void SharedServiceManager::release_handler(EventHandlerBase *) {
    std::lock_guard<std::mutex> lg(mtx);
    if(--handler_ref == 0)
        cv.notify_all();
}

bool SharedServiceManager::start_watch_pods(const WatchConfig &cfg, EventHandlerBase *hdl) {
    WatchContext *ctx = nullptr;

    {
        lock_guard<mutex> lg(mtx);
        if(!running)
            return false;

        ctx = new WatchContext;
        ctx->total_cnt = 1;
        ctx->protector = this;
        ctx->config = new WatchConfig(cfg);

        cfg_hdl.emplace(ctx->config, hdl);
        ++series_ref;
    }

    // we are not in the mutex now

    // use shared ptr to ensure that *this is destroied after series callback
    shared_ptr<SharedServiceManager> shared = this->shared_from_this();

    // start before create task, maybe we need to call on_error
    hdl->on_start_watch(ctx->config);

    auto *task = create_k8s_http_task(ctx);
    auto *series = Workflow::create_series_work(task, nullptr);
    series->set_context(ctx);
    series->set_callback([shared](const SeriesWork *series) mutable {
        shared->series_done(series);
        shared.reset(); // release *this earlier
    });

    series->start();

    return true;
}

void SharedServiceManager::shutdown() {
    unique_lock<mutex> lk(mtx);
    running = false;
    cv.wait(lk, [this]() { return this->handler_ref == 0; });

    auto m = move(cfg_hdl);
    cfg_hdl.clear();
    lk.unlock();

    // stop watch for all handlers, without mutex
    for(auto &p : m)
        p.second->on_stop_watch(p.first);
}

void SharedServiceManager::wait_finish() {
    unique_lock<mutex> lk(mtx);
    cv.wait(lk, [this]() { return this->series_ref == 0; });
}

bool SharedServiceManager::wait_finish_for(double sec) {
    auto dur = chrono::duration<double>(sec);
    unique_lock<mutex> lk(mtx);
    return cv.wait_for(lk, dur, [this]() { return this->series_ref == 0; });
}

void SharedServiceManager::series_done(const SeriesWork *series) {
    auto *ctx = (WatchContext *)series->get_context();
    auto *cfg = ctx->config;
    EventHandlerBase *hdl = nullptr;

    mtx.lock();
    auto it = cfg_hdl.find(cfg);
    if(it != cfg_hdl.end()) {
        hdl = it->second;
        cfg_hdl.erase(it);
    }
    mtx.unlock();

    if(hdl)
        hdl->on_stop_watch(cfg);

    delete ctx->config;
    delete ctx;

    mtx.lock();
    if(--series_ref == 0)
        cv.notify_all();
    mtx.unlock();
}

// ServiceManager

ServiceManager::ServiceManager() : m(new SharedServiceManager) {}
ServiceManager::~ServiceManager() {}

bool ServiceManager::start_watch_pods(const WatchConfig &cfg, EventHandlerBase *hdl) {
    return m->start_watch_pods(cfg, hdl);
}

void ServiceManager::shutdown() {
    m->shutdown();
}

void ServiceManager::wait_finish() {
    m->wait_finish();
}

bool ServiceManager::wait_finish_for(double sec) {
    return m->wait_finish_for(sec);
}

} // namespace k8s
