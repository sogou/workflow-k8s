#include <string>
#include <vector>
#include <memory>
#include "K8sGlobal.h"

#if defined __has_include
#   if __has_include (<jsoncpp/json/json.h>)
#       include <jsoncpp/json/json.h>
#   else
#       include <json/json.h>
#   endif
#else
#   include <jsoncpp/json/json.h>
#endif

namespace k8s {

using Json::Value;
using std::string;
using std::unique_ptr;
using std::move;

static string get_string(const Value &root, const string &key, const string &dft = "") {
    if(root.isObject()) {
        const Value *v = root.find(key.c_str(), key.c_str() + key.size());
        if(v && v->isString())
            return v->asString();
    }
    return dft;
}

static bool get_bool(const Value &root, const string &key, bool dft = false) {
    if(root.isObject()) {
        const Value *v = root.find(key.c_str(), key.c_str() + key.size());
        if(v && v->isBool())
            return v->asBool();
    }
    return dft;
}

static int get_int(const Value &root, const string &key, int dft = 0) {
    if(root.isObject()) {
        const Value *v = root.find(key.c_str(), key.c_str() + key.size());
        if(v && v->isInt())
            return v->asInt();
    }
    return dft;
};

static void parse_metadata(const Value &jmeta, ObjectMeta &meta) {
    if(jmeta.isObject()) {
        meta.name               = get_string(jmeta, "name");
        meta.generate_name      = get_string(jmeta, "generateName");
        meta.name_space         = get_string(jmeta, "namespace");
        meta.self_link          = get_string(jmeta, "selfLink");
        meta.uid                = get_string(jmeta, "uid");
        meta.resource_version   = get_string(jmeta, "resourceVersion");
        meta.creation_timestamp = get_string(jmeta, "creationTimestamp");
        meta.deletion_timestamp = get_string(jmeta, "deletionTimestamp");

        if(jmeta.isMember("labels")) {
            const Value &jlabels = jmeta["labels"];
            for(auto it = jlabels.begin(); it != jlabels.end(); ++it) {
                const Value &key = it.key();
                const Value &value = *it;

                if(key.isString() && value.isString())
                    meta.labels.emplace_back(key.asString(), value.asString());
            }
        }
    }
}

static void parse_status(const Value &jstatus, PodStatus &status) {
    if(jstatus.isObject()) {
        status.phase      = get_string(jstatus, "phase");
        status.host_ip    = get_string(jstatus, "hostIP");
        status.pod_ip     = get_string(jstatus, "podIP");
        status.start_time = get_string(jstatus, "startTime");
        status.reason     = get_string(jstatus, "reason");

        if(jstatus.isMember("podIPs")) {
            const Value &podips = jstatus["PodIPs"];
            for(int i = 0; i < (int)podips.size(); i++) {
                string ip = get_string(podips[i], "ip");
                if(!ip.empty())
                    status.pod_ips.push_back(move(ip));
            }
        }

        if(jstatus.isMember("conditions")) {
            const Value &jconditions = jstatus["conditions"];
            for(int i = 0; i < (int)jconditions.size(); i++) {
                status.conditions.emplace_back();
                PodCondition &pc = status.conditions.back();
                const Value &c = jconditions[i];

                pc.type                 = get_string(c, "type");
                pc.status               = get_string(c, "status");
                pc.reason               = get_string(c, "reason");
                pc.message              = get_string(c, "message");
                pc.last_probe_time      = get_string(c, "lastProbeTime");
                pc.last_transition_time = get_string(c, "lastTransitionTime");
            }
        }
        if(jstatus.isMember("containerStatuses")) {
            const Value &jcs = jstatus["containerStatuses"];
            for(int i = 0; i < (int)jcs.size(); i++) {
                status.container_statuses.emplace_back();
                ContainerStatus &cs = status.container_statuses.back();
                const Value &c = jcs[i];

                cs.ready         = get_bool(c, "ready");
                cs.restart_count = get_int(c, "restartCount");
                cs.name          = get_string(c, "name");
                // cs.state         = get_string(c, "state");
                cs.image         = get_string(c, "image");
                cs.image_id      = get_string(c, "imageID");
                cs.container_id  = get_string(c, "containerID");
            }
        }
    }
}

static void parse_pod(const Value &object, Pod &pod) {
    if(object.isObject()) {
        if(object.isMember("metadata"))
            parse_metadata(object["metadata"], pod.metadata);

        if(object.isMember("status"))
            parse_status(object["status"], pod.status);

        if(pod.type == "ERROR") {
            pod.error_status.code    = get_int(object, "code");
            pod.error_status.status  = get_int(object, "status");
            pod.error_status.message = get_int(object, "message");
            pod.error_status.reason  = get_int(object, "reason");
        }
    }
}

bool parse_json_to_pod(const char *first, const char *last, Pod &pod, string &errors) {
    Json::CharReaderBuilder builder; // TODO make it thread local?
    Value root;

    unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(reader->parse(first, last, &root, &errors) == false)
        return false;

    if(!root.isObject()) {
        errors.assign("Json is not an object");
        return false;
    }

    pod.type = get_string(root, "type");

    if(root.isMember("object") == false)
        return false;

    parse_pod(root["object"], pod);
    return true;
}

bool parse_json_to_podlist(const char *first, const char *last, PodList &podlist, string &errors) {
    Json::CharReaderBuilder builder; // todo make it simple
    Value root;

    unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if(reader->parse(first, last, &root, &errors) == false)
        return false;
    if(!root.isObject()) {
        errors.assign("Json is not an object");
        return false;
    }

    if(root.isMember("metadata")) {
        const Value &meta = root["metadata"];
        podlist.selfLink = get_string(meta, "selfLink");
        podlist.resource_version = get_string(meta, "resourceVersion");
    }

    if(root.isMember("items")) {
        const Value &items = root["items"];
        podlist.items.resize(items.size());
        for(int i = 0; i < (int)items.size(); i++) {
            parse_pod(items[i], podlist.items[i]);
            podlist.items[i].type = "ADDED";
        }
    }
    return true;
}

} // namespace k8s
