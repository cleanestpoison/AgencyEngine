#include "State.h"

namespace AgencyEngine::StateStore
{
    namespace
    {
        std::mutex g_mutex;
        Status     g_status;
    }

    std::mutex& Mutex()
    {
        return g_mutex;
    }

    Status& Raw()
    {
        return g_status;
    }
}
