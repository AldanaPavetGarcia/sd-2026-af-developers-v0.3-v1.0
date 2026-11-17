#include "redisNodeRegistry.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <sys/time.h>
#include <utility>

#include <hiredis/hiredis.h>

namespace
{
    constexpr int MS_PER_SECOND = 1000;
    constexpr int US_PER_MS = 1000;
    constexpr const char* ONLINE = "ONLINE";
    constexpr const char* OFFLINE = "OFFLINE";
    constexpr const char* SOURCE_TCP = "tcp";
    // Return values of REGISTER_SCRIPT.
    constexpr long long REGISTER_NEW = 0;
    constexpr long long REGISTER_RECONNECTED = 1;
    // Return values of TELEMETRY_SCRIPT.
    constexpr long long TELEMETRY_STORED = 0;
    constexpr long long TELEMETRY_OLDER = 1;
    constexpr const char* SOURCE_EDGE = "edge";

    // REGISTER, atomic in Redis (ADR-027 §2). Returns 0 new, 1 reconnected / taken over, 2 duplicate.
    // KEYS: node hash, index set. ARGV: node_id, bunker_name, ip_address, capacity, last_seen,
    // session_id, owner, lease key prefix.
    constexpr const char* REGISTER_SCRIPT = R"lua(
local n = redis.call('HMGET', KEYS[1], 'bunker_name', 'capacity', 'status', 'owner', 'source')
if not n[1] then
  redis.call('HSET', KEYS[1], 'node_id', ARGV[1], 'bunker_name', ARGV[2], 'ip_address', ARGV[3],
             'capacity', ARGV[4], 'status', 'ONLINE', 'last_seen', ARGV[5], 'session_id', ARGV[6],
             'owner', ARGV[7], 'source', 'tcp')
  redis.call('SADD', KEYS[2], ARGV[1])
  return 0
end
if n[5] == 'edge' then return 2 end
local alive = n[3] == 'ONLINE' and n[4] and n[4] ~= '' and
              redis.call('EXISTS', ARGV[8] .. n[4]) == 1
if not alive and n[1] == ARGV[2] and n[2] == ARGV[4] then
  redis.call('HSET', KEYS[1], 'status', 'ONLINE', 'last_seen', ARGV[5], 'ip_address', ARGV[3],
             'session_id', ARGV[6], 'owner', ARGV[7])
  return 1
end
return 2
)lua";

    // TELEMETRY of an edge device (ADR-028 §1). Returns 0 stored, 1 older sample (last_seen only),
    // 2 node registered over TCP. KEYS: node hash, index set. ARGV: node_id, bunker_name, last_seen,
    // temperature, device_status, sampled_at.
    constexpr const char* TELEMETRY_SCRIPT = R"lua(
local source = redis.call('HGET', KEYS[1], 'source')
if source and source ~= 'edge' then return 2 end
if not source then
  redis.call('HSET', KEYS[1], 'node_id', ARGV[1], 'ip_address', '0.0.0.0', 'capacity', '0',
             'source', 'edge', 'owner', '', 'session_id', '')
  redis.call('SADD', KEYS[2], ARGV[1])
end
redis.call('HSET', KEYS[1], 'bunker_name', ARGV[2], 'status', 'ONLINE', 'last_seen', ARGV[3])
local shown = tonumber(redis.call('HGET', KEYS[1], 'sampled_at'))
if shown and shown > tonumber(ARGV[6]) then return 1 end
redis.call('HSET', KEYS[1], 'temperature', ARGV[4], 'device_status', ARGV[5], 'sampled_at', ARGV[6])
return 0
)lua";

    // Writes fields only if this replica still owns the node, so a replica that lost a node to
    // another one cannot mark it OFFLINE. KEYS: node hash. ARGV: owner, field, value, ...
    constexpr const char* OWNER_UPDATE_SCRIPT = R"lua(
if redis.call('HGET', KEYS[1], 'owner') ~= ARGV[1] then return 0 end
redis.call('HSET', KEYS[1], unpack(ARGV, 2))
return 1
)lua";

    // One node with its owner's liveness. KEYS: node hash. ARGV: lease key prefix.
    constexpr const char* QUERY_SCRIPT = R"lua(
local h = redis.call('HGETALL', KEYS[1])
if #h == 0 then return nil end
local owner = ''
for i = 1, #h, 2 do if h[i] == 'owner' then owner = h[i + 1] end end
local alive = 0
if owner ~= '' then alive = redis.call('EXISTS', ARGV[1] .. owner) end
h[#h + 1] = 'owner_alive'
h[#h + 1] = tostring(alive)
return h
)lua";

    // Every node, as QUERY_SCRIPT returns each. KEYS: index set. ARGV: node key prefix, lease key prefix.
    constexpr const char* LIST_SCRIPT = R"lua(
local out = {}
local leases = {}
for _, id in ipairs(redis.call('SMEMBERS', KEYS[1])) do
  local h = redis.call('HGETALL', ARGV[1] .. id)
  if #h > 0 then
    local owner = ''
    for i = 1, #h, 2 do if h[i] == 'owner' then owner = h[i + 1] end end
    if owner ~= '' and leases[owner] == nil then
      leases[owner] = redis.call('EXISTS', ARGV[2] .. owner)
    end
    h[#h + 1] = 'owner_alive'
    h[#h + 1] = tostring(owner ~= '' and leases[owner] or 0)
    out[#out + 1] = h
  end
end
return out
)lua";

    // Removes every node of this prefix; tests only. KEYS: index set. ARGV: node key prefix.
    constexpr const char* CLEAR_SCRIPT = R"lua(
for _, id in ipairs(redis.call('SMEMBERS', KEYS[1])) do redis.call('DEL', ARGV[1] .. id) end
redis.call('DEL', KEYS[1])
return 1
)lua";

    std::int64_t toEpochMs(std::chrono::system_clock::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    }

    template<typename T>
    bool parseNumber(const std::string& text, T& out)
    {
        const char* end = text.data() + text.size();
        auto [ptr, ec] = std::from_chars(text.data(), end, out);
        return ec == std::errc() && ptr == end;
    }

    bool parseDouble(const std::string& text, double& out)
    {
        if (text.empty())
        {
            return false;
        }
        char* end = nullptr;
        out = std::strtod(text.c_str(), &end);
        return end == text.c_str() + text.size();
    }

    // Lossless text form of a double, for Redis.
    std::string formatDouble(double value)
    {
        constexpr int kLosslessDigits = 17;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.*g", kLosslessDigits, value);
        return buf;
    }

    timeval toTimeval(int ms)
    {
        timeval tv {};
        tv.tv_sec = ms / MS_PER_SECOND;
        tv.tv_usec = static_cast<suseconds_t>((ms % MS_PER_SECOND) * US_PER_MS);
        return tv;
    }

    std::string replyString(const redisReply* reply)
    {
        if (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS)
        {
            return std::string(reply->str, reply->len);
        }
        if (reply->type == REDIS_REPLY_INTEGER)
        {
            return std::to_string(reply->integer);
        }
        return {};
    }

    // A flat HGETALL-style array (plus owner_alive) to a record with its effective status.
    bool recordFromHash(const redisReply* hash, NodeRecord& out)
    {
        if (hash == nullptr || hash->type != REDIS_REPLY_ARRAY || hash->elements % 2 != 0)
        {
            return false;
        }
        std::unordered_map<std::string, std::string> fields;
        for (size_t i = 0; i + 1 < hash->elements; i += 2)
        {
            fields[replyString(hash->element[i])] = replyString(hash->element[i + 1]);
        }

        NodeRecord record;
        record.nodeId = fields["node_id"];
        record.bunkerName = fields["bunker_name"];
        record.ipAddress = fields["ip_address"];
        record.sessionId = fields["session_id"];
        record.source = fields["source"].empty() ? SOURCE_TCP : fields["source"];
        std::int64_t lastSeenMs = 0;
        if (record.nodeId.empty() || !parseNumber(fields["capacity"], record.capacity) ||
            !parseNumber(fields["last_seen"], lastSeenMs))
        {
            return false;
        }
        record.lastSeen = std::chrono::system_clock::time_point(std::chrono::milliseconds(lastSeenMs));
        if (record.source == SOURCE_EDGE)
        {
            // No session and no owner: the server derives liveness from last_seen (ADR-028).
            record.status = fields["status"] == ONLINE ? NodeStatus::ONLINE : NodeStatus::OFFLINE;
            record.deviceStatus = fields["device_status"];
            if (!fields["sampled_at"].empty() && (!parseNumber(fields["sampled_at"], record.sampledAtMs) ||
                                                  !parseDouble(fields["temperature"], record.temperature)))
            {
                return false;
            }
            out = std::move(record);
            return true;
        }
        // ADR-027 §2: ONLINE needs both the stored status and a live owner.
        const bool ownerAlive = fields["owner_alive"] == "1";
        record.status = (fields["status"] == ONLINE && ownerAlive) ? NodeStatus::ONLINE : NodeStatus::OFFLINE;
        out = std::move(record);
        return true;
    }

    bool readEnvInt(const char* name, int& out, std::string& error)
    {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0')
        {
            return true;
        }
        int parsed = 0;
        if (!parseNumber(std::string(value), parsed) || parsed <= 0)
        {
            error = std::string(name) + " must be a positive integer";
            return false;
        }
        out = parsed;
        return true;
    }

    void readEnvString(const char* name, std::string& out)
    {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0')
        {
            out = value;
        }
    }
} // namespace

void RedisNodeRegistry::ContextDeleter::operator()(redisContext* ctx) const
{
    redisFree(ctx);
}

void RedisNodeRegistry::ReplyDeleter::operator()(redisReply* reply) const
{
    freeReplyObject(reply);
}

RedisNodeRegistry::RedisNodeRegistry(RedisRegistryConfig config)
    : m_config(std::move(config))
{
}

RedisNodeRegistry::~RedisNodeRegistry()
{
    {
        std::lock_guard<std::mutex> lock(m_stopMutex);
        m_stopping = true;
    }
    m_stopCv.notify_all();
    if (m_leaseThread.joinable())
    {
        m_leaseThread.join();
    }
    // Hand the nodes over at once instead of after the lease TTL (graceful shutdown).
    (void)command({"DEL", leaseKey(m_config.replicaName)});
}

bool RedisNodeRegistry::start()
{
    const bool answered = refreshLease();
    m_leaseThread = std::thread(&RedisNodeRegistry::leaseLoop, this);
    return answered;
}

std::string RedisNodeRegistry::nodeKey(const std::string& nodeId) const
{
    return m_config.keyPrefix + ":node:" + nodeId;
}

std::string RedisNodeRegistry::leaseKey(const std::string& replica) const
{
    return m_config.keyPrefix + ":replica:" + replica;
}

std::string RedisNodeRegistry::indexKey() const
{
    return m_config.keyPrefix + ":nodes";
}

bool RedisNodeRegistry::connectLocked() const
{
    if (m_ctx)
    {
        return true;
    }
    const timeval timeout = toTimeval(m_config.commandTimeoutMs);
    ContextPtr ctx(redisConnectWithTimeout(m_config.host.c_str(), m_config.port, timeout));
    if (!ctx || ctx->err != 0 || redisSetTimeout(ctx.get(), timeout) != REDIS_OK)
    {
        return false;
    }
    m_ctx = std::move(ctx);
    // Whatever Redis holds may be older than this replica's nodes (it may have restarted).
    m_needResync = true;
    return true;
}

RedisNodeRegistry::ReplyPtr RedisNodeRegistry::command(const std::vector<std::string>& args) const
{
    std::vector<const char*> argv;
    std::vector<size_t> argvLen;
    argv.reserve(args.size());
    argvLen.reserve(args.size());
    for (const auto& arg : args)
    {
        argv.push_back(arg.data());
        argvLen.push_back(arg.size());
    }

    std::lock_guard<std::mutex> lock(m_connMutex);
    if (!connectLocked())
    {
        m_healthy = false;
        return nullptr;
    }
    ReplyPtr reply(static_cast<redisReply*>(
        redisCommandArgv(m_ctx.get(), static_cast<int>(argv.size()), argv.data(), argvLen.data())));
    if (!reply || m_ctx->err != 0)
    {
        // The connection is unusable after an I/O error or timeout: open a new one next time.
        m_ctx.reset();
        m_healthy = false;
        return nullptr;
    }
    if (reply->type == REDIS_REPLY_ERROR)
    {
        return nullptr;
    }
    m_healthy = true;
    return reply;
}

void RedisNodeRegistry::dropConnectionForTest()
{
    std::lock_guard<std::mutex> lock(m_connMutex);
    m_ctx.reset();
}

RegisterNodeResult RedisNodeRegistry::registerOrReconnectNode(const NodeRecord& record)
{
    ReplyPtr reply = command({"EVAL",
                              REGISTER_SCRIPT,
                              "2",
                              nodeKey(record.nodeId),
                              indexKey(),
                              record.nodeId,
                              record.bunkerName,
                              record.ipAddress,
                              std::to_string(record.capacity),
                              std::to_string(toEpochMs(record.lastSeen)),
                              record.sessionId,
                              m_config.replicaName,
                              leaseKey("")});
    if (!reply || reply->type != REDIS_REPLY_INTEGER)
    {
        return RegisterNodeResult::REGISTRY_UNAVAILABLE;
    }

    RegisterNodeResult result = RegisterNodeResult::DUPLICATE_ONLINE_NODE;
    switch (reply->integer)
    {
        case REGISTER_NEW: result = RegisterNodeResult::NEW_NODE; break;
        case REGISTER_RECONNECTED: result = RegisterNodeResult::RECONNECTED_NODE; break;
        default: break;
    }
    if (result == RegisterNodeResult::NEW_NODE || result == RegisterNodeResult::RECONNECTED_NODE)
    {
        NodeRecord owned = record;
        owned.status = NodeStatus::ONLINE;
        std::lock_guard<std::mutex> lock(m_ownedMutex);
        m_owned[record.nodeId] = std::move(owned);
    }
    return result;
}

TelemetryResult RedisNodeRegistry::upsertTelemetry(const EdgeTelemetry& telemetry)
{
    ReplyPtr reply = command({"EVAL",
                              TELEMETRY_SCRIPT,
                              "2",
                              nodeKey(telemetry.nodeId),
                              indexKey(),
                              telemetry.nodeId,
                              telemetry.bunkerName,
                              std::to_string(toEpochMs(telemetry.receivedAt)),
                              formatDouble(telemetry.temperature),
                              telemetry.deviceStatus,
                              std::to_string(telemetry.sampledAtMs)});
    if (!reply || reply->type != REDIS_REPLY_INTEGER)
    {
        return TelemetryResult::UNAVAILABLE;
    }
    switch (reply->integer)
    {
        case TELEMETRY_STORED: return TelemetryResult::STORED;
        case TELEMETRY_OLDER: return TelemetryResult::OLDER_SAMPLE;
        default: return TelemetryResult::TCP_NODE;
    }
}

RegistryResult RedisNodeRegistry::queryNode(const std::string& nodeId, NodeRecord& out) const
{
    ReplyPtr reply = command({"EVAL", QUERY_SCRIPT, "1", nodeKey(nodeId), leaseKey("")});
    if (!reply)
    {
        return RegistryResult::UNAVAILABLE;
    }
    if (reply->type == REDIS_REPLY_NIL)
    {
        return RegistryResult::NOT_FOUND;
    }
    return recordFromHash(reply.get(), out) ? RegistryResult::OK : RegistryResult::NOT_FOUND;
}

RegistryResult RedisNodeRegistry::listNodes(std::vector<NodeRecord>& out) const
{
    out.clear();
    ReplyPtr reply = command({"EVAL", LIST_SCRIPT, "1", indexKey(), nodeKey(""), leaseKey("")});
    if (!reply || reply->type != REDIS_REPLY_ARRAY)
    {
        return RegistryResult::UNAVAILABLE;
    }
    out.reserve(reply->elements);
    for (size_t i = 0; i < reply->elements; ++i)
    {
        NodeRecord record;
        if (recordFromHash(reply->element[i], record))
        {
            out.push_back(std::move(record));
        }
    }
    return RegistryResult::OK;
}

bool RedisNodeRegistry::ownerUpdate(const std::string& nodeId, const std::vector<std::string>& fieldValues)
{
    std::vector<std::string> args = {"EVAL", OWNER_UPDATE_SCRIPT, "1", nodeKey(nodeId), m_config.replicaName};
    args.insert(args.end(), fieldValues.begin(), fieldValues.end());
    ReplyPtr reply = command(args);
    return reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
}

void RedisNodeRegistry::bindSession(const std::string& nodeId, const std::string& sessionId)
{
    const auto now = std::chrono::system_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_ownedMutex);
        auto it = m_owned.find(nodeId);
        if (it != m_owned.end())
        {
            it->second.sessionId = sessionId;
            it->second.lastSeen = now;
        }
    }
    (void)ownerUpdate(nodeId, {"status", ONLINE, "last_seen", std::to_string(toEpochMs(now)), "session_id", sessionId});
}

void RedisNodeRegistry::touch(const std::string& nodeId)
{
    const auto now = std::chrono::system_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_ownedMutex);
        auto it = m_owned.find(nodeId);
        if (it != m_owned.end())
        {
            it->second.lastSeen = now;
        }
    }
    (void)ownerUpdate(nodeId, {"status", ONLINE, "last_seen", std::to_string(toEpochMs(now))});
}

void RedisNodeRegistry::markOffline(const std::string& nodeId)
{
    {
        std::lock_guard<std::mutex> lock(m_ownedMutex);
        m_owned.erase(nodeId);
    }
    (void)ownerUpdate(nodeId,
                      {"status",
                       OFFLINE,
                       "last_seen",
                       std::to_string(toEpochMs(std::chrono::system_clock::now())),
                       "session_id",
                       ""});
}

bool RedisNodeRegistry::isOnline(const std::string& nodeId) const
{
    NodeRecord record;
    return queryNode(nodeId, record) == RegistryResult::OK && record.status == NodeStatus::ONLINE;
}

bool RedisNodeRegistry::healthy() const
{
    return m_healthy.load();
}

void RedisNodeRegistry::clear()
{
    {
        std::lock_guard<std::mutex> lock(m_ownedMutex);
        m_owned.clear();
    }
    (void)command({"EVAL", CLEAR_SCRIPT, "1", indexKey(), nodeKey("")});
}

bool RedisNodeRegistry::writeOwnedNode(const NodeRecord& record)
{
    ReplyPtr hset = command({"HSET",        nodeKey(record.nodeId),
                             "node_id",     record.nodeId,
                             "bunker_name", record.bunkerName,
                             "ip_address",  record.ipAddress,
                             "capacity",    std::to_string(record.capacity),
                             "status",      ONLINE,
                             "last_seen",   std::to_string(toEpochMs(record.lastSeen)),
                             "session_id",  record.sessionId,
                             "owner",       m_config.replicaName,
                             "source",      SOURCE_TCP});
    ReplyPtr sadd = command({"SADD", indexKey(), record.nodeId});
    return hset && sadd;
}

bool RedisNodeRegistry::refreshLease()
{
    ReplyPtr lease = command({"SET", leaseKey(m_config.replicaName), "1", "PX", std::to_string(m_config.leaseTtlMs)});
    if (!lease)
    {
        return false;
    }
    if (!m_needResync.exchange(false))
    {
        return true;
    }

    std::vector<NodeRecord> owned;
    {
        std::lock_guard<std::mutex> lock(m_ownedMutex);
        owned.reserve(m_owned.size());
        for (const auto& entry : m_owned)
        {
            owned.push_back(entry.second);
        }
    }
    for (const auto& record : owned)
    {
        if (!writeOwnedNode(record))
        {
            m_needResync = true;
            return false;
        }
    }
    return true;
}

void RedisNodeRegistry::leaseLoop()
{
    std::unique_lock<std::mutex> lock(m_stopMutex);
    while (!m_stopCv.wait_for(lock, std::chrono::milliseconds(m_config.leaseRefreshMs), [this] { return m_stopping; }))
    {
        lock.unlock();
        (void)refreshLease();
        lock.lock();
    }
}

bool loadRedisRegistryConfig(RedisRegistryConfig& config, std::string& error)
{
    readEnvString("EOP_REDIS_HOST", config.host);
    readEnvString("EOP_REGISTRY_PREFIX", config.keyPrefix);
    readEnvString("HOSTNAME", config.replicaName);
    readEnvString("EOP_REPLICA_NAME", config.replicaName);
    if (!readEnvInt("EOP_REDIS_PORT", config.port, error) ||
        !readEnvInt("EOP_REGISTRY_LEASE_TTL_MS", config.leaseTtlMs, error) ||
        !readEnvInt("EOP_REGISTRY_LEASE_REFRESH_MS", config.leaseRefreshMs, error) ||
        !readEnvInt("EOP_REDIS_TIMEOUT_MS", config.commandTimeoutMs, error))
    {
        return false;
    }
    if (config.replicaName.empty())
    {
        error = "EOP_REPLICA_NAME (or HOSTNAME) is required with EOP_REGISTRY_BACKEND=redis";
        return false;
    }
    if (config.leaseRefreshMs >= config.leaseTtlMs)
    {
        error = "EOP_REGISTRY_LEASE_REFRESH_MS must be shorter than EOP_REGISTRY_LEASE_TTL_MS";
        return false;
    }
    return true;
}
