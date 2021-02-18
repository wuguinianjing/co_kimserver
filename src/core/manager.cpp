#include "manager.h"

#include <signal.h>

#include "net/anet.h"
#include "server.h"
#include "util/set_proc_title.h"
#include "util/util.h"
#include "worker.h"

namespace kim {

void* Manager::m_signal_user_data = nullptr;

Manager::Manager() {
}

Manager::~Manager() {
    destory();
}

void Manager::destory() {
    SAFE_DELETE(m_net);
    SAFE_DELETE(m_logger);
}

void Manager::run() {
    if (m_net != nullptr) {
        m_net->run();
    }
}

bool Manager::init(const char* conf_path) {
    char work_path[MAX_PATH];
    if (!getcwd(work_path, sizeof(work_path))) {
        return false;
    }
    m_node_info.set_work_path(work_path);

    if (!load_config(conf_path)) {
        LOG_ERROR("load config failed! %s", conf_path);
        return false;
    }

    if (!load_logger()) {
        LOG_ERROR("init log failed!");
        return false;
    }

    if (!load_network()) {
        LOG_ERROR("create network failed!");
        return false;
    }

    if (!load_signals()) {
        LOG_ERROR("setup signals failed!");
        return false;
    }

    create_workers();
    set_proc_title("%s", m_conf("server_name").c_str());

    init_timer();
    LOG_INFO("init manager done!");
    return true;
}

void Manager::on_repeat_timer() {
    co_enable_hook_sys();

    restart_workers();
    if (m_net != nullptr) {
        m_net->on_repeat_timer();
    }
}

bool Manager::load_logger() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s",
             m_node_info.work_path().c_str(), m_conf("log_path").c_str());

    m_logger = new Log;
    if (m_logger == nullptr) {
        LOG_ERROR("new log failed!");
        return false;
    }

    if (!m_logger->set_log_path(path)) {
        LOG_ERROR("set log path failed! path: %s", path);
        return false;
    }

    if (!m_logger->set_level(m_conf("log_level").c_str())) {
        LOG_ERROR("invalid log level!");
        return false;
    }

    m_logger->set_worker_index(0);
    m_logger->set_process_type(true);
    LOG_INFO("init logger done!");
    return true;
}

bool Manager::load_network() {
    m_net = new Network(m_logger, Network::TYPE::MANAGER);
    if (m_net == nullptr) {
        LOG_ERROR("new network failed!");
        return false;
    }

    if (!m_net->create_m(m_node_info.mutable_addr_info(), m_conf)) {
        LOG_ERROR("init network failed!");
        SAFE_DELETE(m_net);
        return false;
    }

    LOG_INFO("init network done!");
    return true;
}

bool Manager::load_config(const char* path) {
    CJsonObject conf;
    if (!conf.Load(path)) {
        LOG_ERROR("load json config failed! %s", path);
        return false;
    }

    m_old_conf = m_conf;
    m_conf = conf;
    m_node_info.set_conf_path(path);

    if (m_old_conf.ToString() != m_conf.ToString()) {
        if (m_old_conf.ToString().empty()) {
            m_node_info.set_worker_cnt(str_to_int(m_conf("worker_cnt")));
            m_node_info.set_node_type(m_conf("node_type"));
            m_node_info.mutable_addr_info()->set_node_host(m_conf("node_host"));
            m_node_info.mutable_addr_info()->set_node_port(str_to_int(m_conf("node_port")));
            m_node_info.mutable_addr_info()->set_gate_host(m_conf("gate_host"));
            m_node_info.mutable_addr_info()->set_gate_port(str_to_int(m_conf("gate_port")));
        }
    }

    return true;
}

bool Manager::create_worker(int worker_index) {
    int pid, data_fds[2], ctrl_fds[2];

    if (socketpair(PF_UNIX, SOCK_STREAM, 0, ctrl_fds) < 0) {
        LOG_ERROR("create socket pair failed! %d: %s", errno, strerror(errno));
        return false;
    }

    if (socketpair(PF_UNIX, SOCK_STREAM, 0, data_fds) < 0) {
        LOG_ERROR("create socket pair failed! %d: %s", errno, strerror(errno));
        m_net->close_channel(ctrl_fds);
        return false;
    }

    if ((pid = fork()) == 0) {
        /* child. */
        destory();

        close(ctrl_fds[0]);
        close(data_fds[0]);

        worker_info_t info{0, worker_index, ctrl_fds[1], data_fds[1], m_node_info.work_path()};
        Worker worker(worker_name(worker_index));
        if (!worker.init(&info, m_conf)) {
            exit(EXIT_CHILD_INIT_FAIL);
        }
        worker.run();
        exit(EXIT_CHILD);
    } else if (pid > 0) {
        /* parent. */
        close(ctrl_fds[1]);
        close(data_fds[1]);

        if (!m_net->init_manager_channel(ctrl_fds[0], data_fds[0])) {
            LOG_CRIT("channel fd add event failed! kill child: %d", pid);
            kill(pid, SIGKILL);
            return false;
        }

        m_net->worker_data_mgr()->add_worker_info(
            worker_index, pid, ctrl_fds[0], data_fds[0]);
        LOG_INFO("manager ctrl_fd: %d, data_fd: %d", ctrl_fds[0], data_fds[0]);

        return true;
    } else {
        m_net->close_channel(data_fds);
        m_net->close_channel(ctrl_fds);
        LOG_ERROR("error: %d, %s", errno, strerror(errno));
    }

    return false;
}

void Manager::create_workers() {
    for (int i = 1; i <= m_node_info.worker_cnt(); i++) {
        if (!create_worker(i)) {
            LOG_ERROR("create worker failed! index: %d", i);
        }
    }
}

std::string Manager::worker_name(int index) {
    return format_str("%s_w_%d", m_conf("server_name").c_str(), index);
}

bool Manager::load_signals() {
    m_signal_user_data = this;
    int signals[] = {SIGCHLD, SIGILL, SIGBUS, SIGFPE, SIGKILL};
    for (unsigned int i = 0; i < sizeof(signals) / sizeof(int); i++) {
        if (!signal_set(signals[i])) {
            return false;
        }
    }
    return true;
}

bool Manager::signal_set(int signum) {
    int ret;
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = &signal_handler;

    ret = sigaction(signum, &action, 0);
    if (ret != 0) {
        LOG_WARN("setup signal, signum: %d, error: %d, errstr: %s",
                 signum, errno, strerror(errno));
        if (signum == SIGKILL) {
            return true;
        }
    }
    return (ret == 0);
}

void Manager::signal_handler(int signum) {
    Manager* m = (Manager*)m_signal_user_data;
    m->signal_handler_event(signum);
}

void Manager::signal_handler_event(int signum) {
    if (signum == SIGCHLD) {
        int pid, status, res;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (WIFEXITED(status)) {
                res = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                res = WTERMSIG(status);
            } else if (WIFSTOPPED(status)) {
                res = WSTOPSIG(status);
            }
            LOG_CRIT("child terminated! pid: %d, signal %d, error %d:  res: %d!",
                     pid, signum, status, res);
            restart_worker(pid);
        }
    } else {
        LOG_CRIT("%s terminated by signal %d!", m_conf("server_name").c_str(), signum);
        exit(signum);
    }
}

void Manager::restart_workers() {
    int worker_index;

    while (!m_restart_workers.empty()) {
        worker_index = m_restart_workers.front();
        m_restart_workers.pop();

        LOG_DEBUG("restart worker, index: %d", worker_index);
        if (create_worker(worker_index)) {
            LOG_INFO("restart worker ok! index: %d", worker_index);
        } else {
            LOG_ERROR("create worker failed! index: %d", worker_index);
        }
    }
}

bool Manager::restart_worker(pid_t pid) {
    // int chs[2];
    int worker_index;

    // /* close (manager/worker) channels. */
    // if (m_net->worker_data_mgr()->get_worker_channel(pid, chs)) {
    //     m_net->close_conn(chs[0]);
    //     m_net->close_conn(chs[1]);
    // }

    /* restart worker by worker index. */
    worker_index = m_net->worker_data_mgr()->get_worker_index(pid);
    if (worker_index == -1) {
        LOG_ERROR("can not find pid: %d work info.");
        return false;
    }

    /* work in timer. */
    m_net->worker_data_mgr()->del_worker_info(pid);
    m_restart_workers.push(worker_index);
    return true;
}

}  // namespace kim