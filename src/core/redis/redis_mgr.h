
#ifndef __KIM_REDIS_MGR_H__
#define __KIM_REDIS_MGR_H__

#include <hiredis/hiredis.h>

#include "../coroutines.h"
#include "../libco/co_routine.h"
#include "../libco/co_routine_inner.h"
#include "../server.h"

namespace kim {

class RedisMgr : Logger {
    /* redis info. */
    typedef struct redis_info_s {
        std::string node;
        int max_conn_cnt = 0;
        std::string host;
        int port = 0;
    } redis_info_t;

    /* redis cmd task. */
    typedef struct task_s {
        std::string cmd;             /* redis cmd. */
        stCoRoutine_t* co = nullptr; /* user's coroutine. */
        redisReply* reply = nullptr; /* redis cmd's reply. */
    } task_t;

    /* coroutines arg. */
    typedef struct rds_co_data_s {
        stCoCond_t* cond = nullptr;  /* coroutine cond. */
        stCoRoutine_t* co = nullptr; /* redis conn's coroutine. */
        redis_info_t* ri = nullptr;  /* redis info(host,port...) */
        redisContext* c = nullptr;   /* redis conn. */
        std::queue<task_t*> tasks;   /* tasks wait to be handled. */
        void* privdata = nullptr;    /* user's data. */
    } rds_co_data_t;

    typedef struct rds_co_array_data_s {
        redis_info_t* ri = nullptr; /* redis info(host,port...) */
        std::vector<rds_co_data_t*> coroutines;
    } rds_co_array_data_t;

   public:
    RedisMgr(Log* log);
    virtual ~RedisMgr();

   public:
    bool init(CJsonObject* config);
    redisReply* exec_cmd(const std::string& node, const std::string& cmd);

   private:
    void destory();
    redisContext* connect(const std::string& host, int port);

    void clear_co_tasks(rds_co_data_t* rds_co);
    static void* co_handle_task(void* arg);
    void* handle_task(void* arg);
    redisReply* send_task(const std::string& node, const std::string& cmd);
    rds_co_data_t* get_co_data(const std::string& node, const std::string& obj);
    void co_sleep(int ms);

   private:
    std::unordered_map<std::string, redis_info_t*> m_rds_infos;
    std::unordered_map<std::string, rds_co_array_data_t*> m_coroutines;
};

}  // namespace kim

#endif  //__KIM_REDIS_MGR_H__