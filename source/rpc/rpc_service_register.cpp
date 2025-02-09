//
// Created by zavier on 2022/1/16.
//

#include "acid/log.h"
#include "acid/rpc/rpc.h"
#include "acid/rpc/rpc_service_registry.h"
#include "acid/rpc/rpc_session.h"

namespace acid::rpc {
static Logger::ptr g_logger = ACID_LOG_NAME("system");

RpcServiceRegistry::RpcServiceRegistry(IOManager *worker, IOManager *accept_worker)
        : TcpServer(worker, accept_worker) {
    setName("RpcServiceRegistry");
}

void RpcServiceRegistry::update(Timer::ptr& heartTimer, Socket::ptr client) {
    ACID_LOG_DEBUG(g_logger) << "update heart";
    if (!heartTimer) {
        heartTimer = m_worker->addTimer(m_AliveTime, [client]{
            ACID_LOG_DEBUG(g_logger) << "client:" << client->toString() << " closed";
            client->close();
        });
        return;
    }
    // 更新定时器
    heartTimer->reset(m_AliveTime, true);
}

void RpcServiceRegistry::handleClient(Socket::ptr client) {
    ACID_LOG_DEBUG(g_logger) << "handleClient: " << client->toString();
    RpcSession::ptr session = std::make_shared<RpcSession>(client);
    Timer::ptr heartTimer;
    // 开启心跳定时器
    update(heartTimer, client);

    Address::ptr providerAddr;
    while (true) {
        Protocol::ptr request = session->recvProtocol();
        if (!request) {
            if (providerAddr) {
                ACID_LOG_WARN(g_logger) << client->toString() << " was closed; unregister " << providerAddr->toString();
                handleUnregisterService(providerAddr);
            }
            return;
        }
        // 更新定时器
        update(heartTimer, client);

        Protocol::ptr response;
        Protocol::MsgType type = request->getMsgType();

        switch (type) {
            case Protocol::MsgType::HEARTBEAT_PACKET:
                response = handleHeartbeatPacket(request);
                break;
            case Protocol::MsgType::RPC_PROVIDER:
                ACID_LOG_DEBUG(g_logger) << client->toString();
                providerAddr = handleProvider(request, client);
                continue;
            case Protocol::MsgType::RPC_SERVICE_REGISTER:
                response = handleRegisterService(request, providerAddr);
                break;
            case Protocol::MsgType::RPC_SERVICE_DISCOVER:
                response = handleDiscoverService(request);
                break;
            default:
                ACID_LOG_WARN(g_logger) << "protocol:" << request->toString();
                continue;
        }

        session->sendProtocol(response);
    }
}

Protocol::ptr RpcServiceRegistry::handleHeartbeatPacket(Protocol::ptr p) {
    return Protocol::HeartBeat();
}

Address::ptr RpcServiceRegistry::handleProvider(Protocol::ptr p, Socket::ptr sock){
    uint32_t port = 0;
    Serializer s(p->getContent());
    s.reset();
    s >> port;
    IPv4Address::ptr address(new IPv4Address(*std::dynamic_pointer_cast<IPv4Address>(sock->getRemoteAddress())));
    address->setPort(port);
    return address;
}

Protocol::ptr RpcServiceRegistry::handleRegisterService(Protocol::ptr p, Address::ptr address) {
    std::string serviceAddress = address->toString();
    std::string serviceName = p->getContent();

    RWMutexType::WriteLock lock(m_mutex);
    auto it = m_services.emplace(serviceName, serviceAddress);
    m_iters[serviceAddress].push_back(it);
    lock.unlock();

    Result<std::string> res = Result<std::string>::Success();
    res.setVal(serviceName);

    Serializer s;
    s << res;
    s.reset();

    Protocol::ptr proto = Protocol::Create(Protocol::MsgType::RPC_SERVICE_REGISTER_RESPONSE, s.toString());
    return proto;
}

void RpcServiceRegistry::handleUnregisterService(Address::ptr address) {
    RWMutexType::WriteLock lock(m_mutex);
    auto it = m_iters.find(address->toString());
    if (it == m_iters.end()) {
        return;
    }
    auto its = it->second;
    for (auto& i: its) {
        m_services.erase(i);
    }
    m_iters.erase(address->toString());
}

Protocol::ptr RpcServiceRegistry::handleDiscoverService(Protocol::ptr p) {
    std::string serviceName = p->getContent();
    std::vector<Result<std::string>> result;
    ByteArray byteArray;

    RWMutexType::ReadLock lock(m_mutex);
    m_services.equal_range(serviceName);
    auto range = m_services.equal_range(serviceName);
    uint32_t cnt = 0;
    // 未注册服务
    if (range.first == range.second) {
        cnt++;
        Result<std::string> res;
        res.setCode(RPC_NO_METHOD);
        res.setMsg("discover service:" + serviceName);
        result.push_back(res);
    } else {
        for (auto i = range.first; i != range.second; ++i) {
            Result<std::string> res;
            std::string addr;
            res.setCode(RPC_SUCCESS);
            res.setVal(i->second);
            result.push_back(res);
        }
        cnt = result.size();
    }

    Serializer s;
    s << cnt;
    for (uint32_t i = 0; i < cnt; ++i) {
        s << result[i];
    }
    s.reset();
    Protocol::ptr proto = Protocol::Create(Protocol::MsgType::RPC_SERVICE_DISCOVER_RESPONSE, s.toString());
    return proto;
}


}