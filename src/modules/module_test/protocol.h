#ifndef __KIM_PROTOCOL_H__
#define __KIM_PROTOCOL_H__

namespace kim {

enum {
    KP_REQ_TEST_HELLO = 1001,
    KP_RSP_TEST_HELLO = 1002,
    KP_REQ_TEST_MYSQL = 1003,
    KP_RSP_TEST_MYSQL = 1004,
    KP_REQ_TEST_REDIS = 1005,
    KP_RSP_TEST_REDIS = 1006,
};

}

#endif  //__KIM_PROTOCOL_H__