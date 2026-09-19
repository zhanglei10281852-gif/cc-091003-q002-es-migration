#include "migration.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace es {

#include <cstdlib>

namespace {
constexpr const char* META_STAGE = "migration_stage";
constexpr const char* META_ID = "migration_id";
constexpr const char* META_SOURCE = "source_index";
constexpr const char* META_BASE = "base_name";
constexpr const char* META_REASON = "failure_reason";

std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// 读取索引 mapping 的 _meta（可用任意客户端调用，线程安全前提是调用方独占该客户端）
json readMappingMeta(ESClient& cli, const std::string& index) {    auto resp = cli.rawRequest("GET", "/" + urlEncode(index) + "/_mapping");
    if (!resp.isSuccess()) {
        throw ESException("Failed to read mapping of '" + index + "': " +
                          resp.body);
    }
    auto body = json::parse(resp.body);
    auto it = body.find(index);
    if (it == body.end() || !it->contains("mappings")) {
        return json::object();
    }
    return it->value("mappings", json::object())
        .value("_meta", json::object());
}
} // namespace

// ==================== 构造与析构 ====================

IndexMigration::IndexMigration(ESClient& client,
                               std::string baseName,
                               json newMapping,
                               json newSettings)
    : client_(client),
      base_(std::move(baseName)),
      newMapping_(std::move(newMapping)),
      newSettings_(std::move(newSettings)),
      readAlias_(base_ + "-read"),
      writeAlias_(base_ + "-write") {}

IndexMigration::~IndexMigration() {
    stopCopyThread();
}

std::string IndexMigration::stageToString(MigrationStage stage) {
    switch (stage) {
        case MigrationStage::NOT_STARTED: return "NOT_STARTED";
        case MigrationStage::COPYING: return "COPYING";
        case MigrationStage::FINAL_SYNC: return "FINAL_SYNC";
        case MigrationStage::VERIFIED: return "VERIFIED";
        case MigrationStage::SWITCHED: return "SWITCHED";
        case MigrationStage::ROLLED_BACK: return "ROLLED_BACK";
        case MigrationStage::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

std::string IndexMigration::versionedName(int version) const {
    std::ostringstream oss;
    oss << base_ << "-v" << std::setw(6) << std::setfill('0') << version;
    return oss.str();
}

int IndexMigration::versionOf(const std::string& indexName,
                              const std::string& base) {
    const std::string prefix = base + "-v";
    if (indexName.rfind(prefix, 0) != 0) {
        return -1;
    }
    const std::string tail = indexName.substr(prefix.size());
    if (tail.empty() || !std::all_of(tail.begin(), tail.end(), ::isdigit)) {
        return -1;
    }
    return std::stoi(tail);
}

std::string IndexMigration::generateMigrationId() {
    static thread_local std::mt19937_64 rng{
        static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count())};
    std::ostringstream oss;
    oss << "mig-" << std::hex << rng() << rng();
    return oss.str();
}

std::vector<std::string> IndexMigration::listVersionIndices() {
    std::vector<std::string> result;
    auto resp = client_.rawRequest("GET", "/_cat/indices/" +
                                    urlEncode(base_ + "-v*") +
                                    "?h=index&format=json");
    if (resp.isNotFound()) {
        return result;
    }
    if (!resp.isSuccess()) {
        throw ESException("Failed to list version indices: " + resp.body);
    }
    auto arr = json::parse(resp.body);
    for (const auto& row : arr) {
        const std::string name = row.value("index", "");
        if (versionOf(name, base_) >= 0) {
            result.push_back(name);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// ==================== _meta 状态持久化 ====================

json IndexMigration::readMeta(const std::string& index) {
    auto resp = client_.rawRequest("GET", "/" + urlEncode(index) + "/_mapping");
    if (!resp.isSuccess()) {
        throw ESException("Failed to read mapping of '" + index + "': " +
                          resp.body);
    }
    auto body = json::parse(resp.body);
    auto it = body.find(index);
    if (it == body.end() || !it->contains("mappings")) {
        return json::object();
    }
    return it->value("mappings", json::object())
        .value("_meta", json::object());
}

void IndexMigration::writeMeta(const std::string& index, const json& meta) {
    // ES 支持整体替换某个索引 mapping 的 _meta
    json body = {{"_meta", meta}};
    auto resp = client_.rawRequest("PUT",
                                   "/" + urlEncode(index) + "/_mapping",
                                   body.dump());
    if (!resp.isSuccess()) {
        throw ESException("Failed to write migration meta on '" + index +
                          "': " + resp.body);
    }
}

void IndexMigration::writeStage(const std::string& targetIndex,
                                MigrationStage stage,
                                const std::string& reason) {
    std::lock_guard<std::mutex> lock(metaMutex_);
    json meta = readMeta(targetIndex);
    meta[META_STAGE] = stageToString(stage);
    if (!reason.empty()) {
        meta[META_REASON] = reason;
    } else if (stage != MigrationStage::FAILED) {
        meta.erase(META_REASON);
    }
    writeMeta(targetIndex, meta);
}

// ==================== 状态发现（恢复的核心） ====================

IndexMigration::DiscoveredState IndexMigration::discover() {
    DiscoveredState state;

    // 写别名当前指向（若存在）即为"现役"索引
    const std::string writeTarget = client_.resolveAlias(writeAlias_);

    // 列出所有版本索引，从最新版本向前找带迁移 _meta 的索引
    auto versions = listVersionIndices();
    for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
        json meta;
        try {
            meta = readMeta(*it);
        } catch (const ESException&) {
            continue;
        }
        if (!meta.contains(META_ID)) {
            continue;
        }

        state.found = true;
        state.migrationId = meta.value(META_ID, "");
        state.targetIndex = *it;
        state.sourceIndex = meta.value(META_SOURCE, "");
        state.failureReason = meta.value(META_REASON, "");
        state.meta = meta;

        const std::string stageStr = meta.value(META_STAGE, "COPYING");
        if (stageStr == "COPYING") state.stage = MigrationStage::COPYING;
        else if (stageStr == "FINAL_SYNC") state.stage = MigrationStage::FINAL_SYNC;
        else if (stageStr == "VERIFIED") state.stage = MigrationStage::VERIFIED;
        else if (stageStr == "SWITCHED") state.stage = MigrationStage::SWITCHED;
        else if (stageStr == "ROLLED_BACK") state.stage = MigrationStage::ROLLED_BACK;
        else if (stageStr == "FAILED") state.stage = MigrationStage::FAILED;
        else state.stage = MigrationStage::COPYING;
        return state;
    }

    // 没有进行中的迁移：若写别名指向某个版本索引，则把它视作基线源
    if (!writeTarget.empty()) {
        state.sourceIndex = writeTarget;
    }
    return state;
}

MigrationStatus IndexMigration::buildStatus(const DiscoveredState& state) {
    MigrationStatus status;
    status.migrationId = state.migrationId;
    status.stage = state.stage;
    status.baseName = base_;
    status.sourceIndex = state.sourceIndex;
    status.targetIndex = state.targetIndex;
    status.failureReason = state.failureReason;
    status.readAlias = readAlias_;
    status.writeAlias = writeAlias_;
    status.writeAliasPointsTo = client_.resolveAlias(writeAlias_);

    auto readMap = client_.getAliases(readAlias_);
    auto rit = readMap.find(readAlias_);
    if (rit != readMap.end()) {
        status.readAliasPointsTo = rit->second;
        std::sort(status.readAliasPointsTo.begin(),
                  status.readAliasPointsTo.end());
    }

    if (state.found) {
        if (client_.indexExists(state.sourceIndex)) {
            status.progress.sourceCount = client_.documentCount(state.sourceIndex);
        }
        if (client_.indexExists(state.targetIndex)) {
            status.progress.targetCount = client_.documentCount(state.targetIndex);
        }
        status.progress.copiedInProcess = copiedInProcess_.load();
        status.progress.failedInProcess = failedInProcess_.load();
        status.progress.copyThreadAlive = copyThread_.joinable();
    }
    return status;
}

MigrationStatus IndexMigration::getStatus() {
    return buildStatus(discover());
}

// ==================== 发起 / 恢复迁移 ====================

MigrationStatus IndexMigration::start() {
    DiscoveredState state = discover();

    if (state.found) {
        // 上一次迁移已切换、写流量确在新版本上：把新版本作为下一轮迁移的
        // 基线（旧版本是否已清理不阻塞，清理是独立的收尾动作）
        if (state.stage == MigrationStage::SWITCHED &&
            client_.resolveAlias(writeAlias_) == state.targetIndex) {
            state.found = false;
            state.stage = MigrationStage::NOT_STARTED;
            state.sourceIndex = state.targetIndex;
            state.targetIndex.clear();
            state.migrationId.clear();
        }
    }

    if (state.found) {
        // 已有迁移：按阶段恢复，绝不重复创建版本
        switch (state.stage) {
            case MigrationStage::COPYING:
            case MigrationStage::FINAL_SYNC:
                // 重启后 FINAL_SYNC 可能是上次崩溃留下的：源索引仍冻结，
                // 直接继续完成冻结对账与切换
                if (!copyThread_.joinable()) {
                    startCopyThread(state.sourceIndex, state.targetIndex);
                }
                break;
            case MigrationStage::VERIFIED:
            case MigrationStage::SWITCHED:
            case MigrationStage::ROLLED_BACK:
            case MigrationStage::FAILED:
                break;  // 交给 verifyAndSwitch / rollback 处理
            case MigrationStage::NOT_STARTED:
                break;
        }
        return buildStatus(state);
    }

    // ---------- 全新迁移 ----------
    std::string sourceIndex;
    if (!state.sourceIndex.empty()) {
        // 写别名已指向某个现役版本索引
        sourceIndex = state.sourceIndex;
    } else {
        // 首次迁移：若存在与 base 同名的物理索引（旧程序写死的索引名），
        // 以它为 v1 基线；否则创建空的 v1
        const std::string legacy = base_;
        if (client_.indexExists(legacy)) {
            sourceIndex = legacy;
        } else {
            const std::string v1 = versionedName(1);
            if (!client_.indexExists(v1)) {
                client_.createIndex(v1, newMapping_, newSettings_);
                client_.refreshIndex(v1);
            }
            sourceIndex = v1;
        }
    }

    int sourceVersion = versionOf(sourceIndex, base_);
    int nextVersion = (sourceVersion >= 0) ? sourceVersion + 1 : 1;
    const std::string targetIndex = versionedName(nextVersion);

    if (client_.indexExists(targetIndex)) {
        // 同名版本已存在但没有迁移 _meta：不属于本流程创建的半成品，
        // 不能覆盖
        throw ESException("Target index '" + targetIndex +
                          "' already exists without migration metadata; "
                          "refusing to create a duplicate version");
    }

    // 用"目标 mapping 中追加 _meta"的方式创建新版本（_meta 在创建时即可写入）
    json targetMappings = newMapping_;
    const std::string migrationId = generateMigrationId();
    targetMappings["_meta"] = {
        {META_ID, migrationId},
        {META_STAGE, "COPYING"},
        {META_SOURCE, sourceIndex},
        {META_BASE, base_}
    };
    client_.createIndex(targetIndex, targetMappings, newSettings_);

    // 确保别名挂在源索引上（读、写均指向源；写别名唯一且可写）
    json aliasActions = {{"actions", json::array()}};
    auto ensureAlias = [&](const std::string& alias, bool writable) {
        const std::string current = client_.resolveAlias(alias);
        if (current != sourceIndex) {
            if (!current.empty()) {
                aliasActions["actions"].push_back(
                    {{"remove", {{"index", current}, {"alias", alias}}}});
            }
            json add = {{"index", sourceIndex}, {"alias", alias}};
            if (writable) {
                add["is_write_index"] = true;
            }
            aliasActions["actions"].push_back({{"add", add}});
        }
    };
    ensureAlias(readAlias_, false);
    ensureAlias(writeAlias_, true);
    if (!aliasActions["actions"].empty()) {
        client_.updateAliases(aliasActions);
    }

    copiedInProcess_ = 0;
    failedInProcess_ = 0;
    startCopyThread(sourceIndex, targetIndex);

    client_.emitLog("迁移已发起: id=" + migrationId + " " + sourceIndex +
                    " -> " + targetIndex);
    return buildStatus(discover());
}

// ==================== 后台复制 ====================

void IndexMigration::startCopyThread(const std::string& source,
                                     const std::string& target) {
    std::lock_guard<std::mutex> lock(threadMutex_);
    stopFlag_ = false;
    copyThread_ = std::thread(&IndexMigration::copyLoop, this,
                              source, target);
}

void IndexMigration::stopCopyThread() {
    {
        std::lock_guard<std::mutex> lock(threadMutex_);
        stopFlag_ = true;
        if (copyThread_.joinable()) {
            copyThread_.join();
        }
    }
}

void IndexMigration::scrollAll(ESClient& cli,
                               const std::string& index,
                               bool includeSource,
                               const BatchHandler& handler) {
    json body = {
        {"size", batchSize_},
        {"sort", json::array({"_doc"})},
        {"query", {{"match_all", json::object()}}}
    };
    if (!includeSource) {
        body["_source"] = false;
    }

    auto resp = cli.rawRequest(
        "POST", "/" + urlEncode(index) + "/_search?scroll=2m", body.dump());
    if (!resp.isSuccess()) {
        throw ESException("scroll initial search failed: " + resp.body);
    }
    auto data = json::parse(resp.body);
    std::string scrollId = data.value("_scroll_id", "");

    try {
        while (true) {
            const auto& hits = data["hits"]["hits"];
            if (hits.empty()) {
                break;
            }
            std::vector<SearchHit> batch;
            batch.reserve(hits.size());
            for (const auto& h : hits) {
                SearchHit hit;
                hit.id = h.value("_id", "");
                hit.index = h.value("_index", "");
                hit.source = h.value("_source", json::object());
                batch.push_back(std::move(hit));
            }
            handler(batch);

            json scrollBody = {{"scroll", "2m"}, {"scroll_id", scrollId}};
            auto next = cli.rawRequest("POST", "/_search/scroll",
                                       scrollBody.dump());
            if (!next.isSuccess()) {
                throw ESException("scroll continuation failed: " + next.body);
            }
            data = json::parse(next.body);
        }
    } catch (...) {
        if (!scrollId.empty()) {
            json delBody = {{"scroll_id", json::array({scrollId})}};
            cli.rawRequest("DELETE", "/_search/scroll", delBody.dump());
        }
        throw;
    }

    if (!scrollId.empty()) {
        json delBody = {{"scroll_id", json::array({scrollId})}};
        cli.rawRequest("DELETE", "/_search/scroll", delBody.dump());
    }
}

int IndexMigration::bulkCopyBatch(ESClient& cli,
                                  const std::string& target,
                                  const std::vector<SearchHit>& hits,
                                  std::atomic<int64_t>& copied,
                                  std::atomic<int64_t>& failed) {
    if (hits.empty()) {
        return 0;
    }
    std::vector<ESClient::BulkAction> actions;
    actions.reserve(hits.size());
    for (const auto& hit : hits) {
        ESClient::BulkAction action;
        action.op = "index";
        action.index = target;
        action.id = hit.id;
        action.doc = hit.source;
        actions.push_back(std::move(action));
    }
    BulkResult result = cli.bulkActions(actions);
    copied += static_cast<int64_t>(result.successCount);
    failed += static_cast<int64_t>(result.failCount);
    return result.failCount;
}

IndexMigration::ReconcileStats IndexMigration::reconcilePass(
        ESClient& cli,
        const std::string& source,
        const std::string& target,
        std::atomic<int64_t>& copiedCounter,
        std::atomic<int64_t>& failedCounter) {
    ReconcileStats stats;

    // 1) 全量复制源文档（按 _id 覆盖写，天然幂等）
    scrollAll(cli, source, true, [&](const std::vector<SearchHit>& batch) {
        stats.failed += bulkCopyBatch(cli, target, batch,
                                      copiedCounter, failedCounter);
        stats.indexed += static_cast<int64_t>(batch.size());
    });

    // 2) 删除目标中存在、但源中已不存在的文档（处理迁移期间的删除）
    std::unordered_set<std::string> sourceIds;
    scrollAll(cli, source, false, [&](const std::vector<SearchHit>& batch) {
        for (const auto& hit : batch) {
            sourceIds.insert(hit.id);
        }
    });

    scrollAll(cli, target, false, [&](const std::vector<SearchHit>& batch) {
        std::vector<ESClient::BulkAction> actions;
        for (const auto& hit : batch) {
            if (sourceIds.find(hit.id) == sourceIds.end()) {
                actions.push_back(
                    {"delete", target, hit.id, json::object()});
            }
        }
        if (!actions.empty()) {
            BulkResult r = cli.bulkActions(actions);
            stats.deleted += r.successCount;
            stats.failed += r.failCount;
            failedCounter += r.failCount;
        }
    });

    return stats;
}

void IndexMigration::copyLoop(std::string source, std::string target) {
    // 本线程独占的 ES 客户端：libcurl handle 不允许跨线程共享
    std::unique_ptr<ESClient> ownClient = client_.fork();

    // 连续对账：复制幂等，直到发现阶段离开 COPYING（进入 FINAL_SYNC 后，
    // 由 verifyAndSwitch 在冻结状态下执行最后一次对账）
    while (!stopFlag_) {
        try {
            auto meta = readMappingMeta(*ownClient, target);
            const std::string stage = meta.value(META_STAGE, "COPYING");
            if (stage != "COPYING") {
                return;
            }

            // 先 refresh 再对账，保证源/新两侧 id 集合基于同一可见视图，
            // 避免把尚未刷盘的新写入误判为"源已删除"
            ownClient->refreshIndex(source);
            ownClient->refreshIndex(target);
            reconcilePass(*ownClient, source, target,
                          copiedInProcess_, failedInProcess_);

            // 短暂等待后继续下一轮，追赶迁移期间的新增/更新/删除
            for (int waited = 0; waited < 300 && !stopFlag_; waited += 100) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(100));
            }
        } catch (const std::exception& e) {
            ownClient->emitLog(
                std::string("复制循环异常，将重试: ") + e.what());
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

MigrationStatus IndexMigration::waitForCopySettled(int pollIntervalMs,
                                                    int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    DiscoveredState state = discover();
    while (state.found &&
           (state.stage == MigrationStage::COPYING ||
            state.stage == MigrationStage::FINAL_SYNC)) {
        // "稳定"判据：连续两轮对账后两侧文档数相等
        if (state.stage == MigrationStage::COPYING) {
            int64_t src = client_.documentCount(state.sourceIndex);
            int64_t dst = client_.documentCount(state.targetIndex);
            if (src == dst) {
                // 等待一个对账周期，确认数量持续一致后再返回
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(pollIntervalMs));
                state = discover();
                int64_t src2 = client_.documentCount(state.sourceIndex);
                int64_t dst2 = client_.documentCount(state.targetIndex);
                if (state.stage == MigrationStage::COPYING &&
                    src2 == dst2 && src2 == src) {
                    return buildStatus(state);
                }
            }
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return buildStatus(state);
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(pollIntervalMs));
        state = discover();
    }
    return buildStatus(state);
}

// ==================== 校验 ====================

IndexMigration::VerificationReport IndexMigration::verify(
        const DiscoveredState& state) {
    VerificationReport report;

    // 1) 复制失败项（本进程内可见的）
    if (failedInProcess_.load() > 0) {
        report.ok = false;
        report.reason = "存在 " +
            std::to_string(failedInProcess_.load()) +
            " 个复制失败项";
        return report;
    }

    // 2) 文档数量一致
    client_.refreshIndex(state.sourceIndex);
    client_.refreshIndex(state.targetIndex);
    const int64_t sourceCount = client_.documentCount(state.sourceIndex);
    const int64_t targetCount = client_.documentCount(state.targetIndex);
    if (sourceCount != targetCount) {
        report.ok = false;
        report.reason = "文档数量不一致: 源=" +
            std::to_string(sourceCount) + " 新=" +
            std::to_string(targetCount);
        return report;
    }

    // 3) Mapping 核对：期望的新字段定义必须全部存在，且类型一致
    json actual = client_.getMapping(state.targetIndex);
    json actualProps = actual.value("properties", json::object());
    json expectedProps = newMapping_.value("properties", json::object());

    std::function<bool(const std::string&, const json&, const json&,
                       std::string&)> checkProps =
        [&](const std::string& prefix, const json& expected,
            const json& actual2, std::string& err) -> bool {
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            const std::string field = prefix.empty()
                ? it.key() : prefix + "." + it.key();
            if (!actual2.contains(it.key())) {
                err = "新索引缺少字段: " + field;
                return false;
            }
            const json& exp = it.value();
            const json& act = actual2.at(it.key());
            // 叶子字段：比对 type
            if (exp.contains("type")) {
                if (act.value("type", "") != exp.value("type", "")) {
                    err = "字段类型不匹配: " + field + " 期望=" +
                          exp.value("type", "") + " 实际=" +
                          act.value("type", "");
                    return false;
                }
            }
            // 嵌套 properties（如 multi-field / object）递归核对
            if (exp.contains("properties")) {
                if (!act.contains("properties") ||
                    !checkProps(field, exp["properties"],
                                act["properties"], err)) {
                    if (err.empty()) {
                        err = "新索引缺少嵌套字段定义: " + field;
                    }
                    return false;
                }
            }
        }
        return true;
    };

    std::string mappingError;
    if (!checkProps("", expectedProps, actualProps, mappingError)) {
        report.ok = false;
        report.reason = "Mapping 校验失败: " + mappingError;
        return report;
    }

    // 4) 文档级抽检：逐 id 比对 _source（数量相等后确保内容无遗漏/陈旧）
    bool mismatch = false;
    std::string mismatchDetail;
    scrollAll(client_, state.targetIndex, true,
              [&](const std::vector<SearchHit>& batch) {
        if (mismatch) return;
        for (const auto& hit : batch) {
            auto src = client_.getDocument(state.sourceIndex, hit.id);
            if (!src) {
                mismatch = true;
                mismatchDetail = "文档在源中不存在: id=" + hit.id;
                return;
            }
            if (*src != hit.source) {
                mismatch = true;
                mismatchDetail = "文档内容不一致: id=" + hit.id;
                return;
            }
        }
    });
    if (mismatch) {
        report.ok = false;
        report.reason = "复制内容核对失败: " + mismatchDetail;
        return report;
    }

    report.ok = true;
    return report;
}

// ==================== 校验 + 原子切换 ====================

void IndexMigration::pointAliasPairTo(const std::string& index) {
    json actions = json::array();

    for (size_t i = 0; i < 2; ++i) {
        const std::string& alias = (i == 0) ? readAlias_ : writeAlias_;
        const bool writable = (i == 1);

        std::vector<std::string> current;
        auto aliasMap = client_.getAliases(alias);
        auto it = aliasMap.find(alias);
        if (it != aliasMap.end()) {
            current = it->second;
        }

        // 先 remove 既有指向，保证写别名任意时刻只关联一个索引
        for (const auto& cur : current) {
            if (cur != index) {
                actions.push_back(
                    {{"remove", {{"index", cur}, {"alias", alias}}}});
            }
        }

        bool alreadyPoints = std::find(current.begin(), current.end(), index)
                             != current.end();
        if (!alreadyPoints) {
            json add = {{"index", index}, {"alias", alias}};
            if (writable) {
                add["is_write_index"] = true;
            }
            actions.push_back({{"add", add}});
        }
    }

    if (!actions.empty()) {
        client_.updateAliases({{"actions", actions}});
    }
}

MigrationStatus IndexMigration::verifyAndSwitch() {
    DiscoveredState state = discover();

    if (!state.found) {
        throw ESException("没有进行中的迁移，请先调用 start()");
    }

    // 已完成：幂等返回
    if (state.stage == MigrationStage::SWITCHED) {
        return buildStatus(state);
    }

    // 崩溃窗口恢复：别名已原子切到新版本、但元数据还停留在 VERIFIED/
    // FINAL_SYNC（进程在切换后、落元数据前崩溃）。此时流量事实已在新版本，
    // 不能重复冻结对账，直接补齐目标可写状态与元数据
    {
        const std::string writeNow = client_.resolveAlias(writeAlias_);
        auto readMap = client_.getAliases(readAlias_);
        bool readOnTarget = false;
        auto rit = readMap.find(readAlias_);
        if (rit != readMap.end()) {
            readOnTarget = std::find(rit->second.begin(), rit->second.end(),
                                     state.targetIndex) != rit->second.end();
        }
        if (writeNow == state.targetIndex && readOnTarget &&
            state.stage != MigrationStage::ROLLED_BACK) {
            // 源索引可能仍带着冻结期的写阻塞，一并解除以便后续可回退
            try { client_.setIndexWriteBlock(state.sourceIndex, false); }
            catch (const std::exception&) {}
            client_.setIndexWriteBlock(state.targetIndex, false);
            writeStage(state.targetIndex, MigrationStage::SWITCHED);
            client_.emitLog("检测到切换后崩溃的中间态，已确认别名在新版本，"
                            "幂等补齐为 SWITCHED");
            return buildStatus(discover());
        }
    }

    // 已回退：需要显式重新切换（走与首次切换相同的校验）
    if (state.stage == MigrationStage::ROLLED_BACK) {
        // 落到下方冻结 + 对账 + 校验流程
    }
    if (state.stage == MigrationStage::FAILED) {
        // 允许在问题修复后重试：重新走冻结校验流程
    }

    const std::string source = state.sourceIndex;
    const std::string target = state.targetIndex;

    // 停掉后台连续复制，避免冻结期间继续写
    stopCopyThread();

    // 1) 冻结源索引写入（阻塞应用写请求，窗口尽量短）
    try {
        client_.setIndexWriteBlock(source, true);
    } catch (const std::exception& e) {
        // 冻结失败不能继续
        writeStage(target, MigrationStage::FAILED, e.what());
        throw;
    }
    writeStage(target, MigrationStage::FINAL_SYNC);

    // 目标索引可能在之前的回退中被置为只读，重新前进前解除其写阻塞
    try {
        client_.setIndexWriteBlock(target, false);
    } catch (const std::exception&) {
        // 无阻塞设置时该接口可能返回异常，忽略
    }

    // 2) 冻结状态下做最后一次对账（新增/更新/删除全部追平）
    //    计数器在此清零：最终对账的失败项才是切换判据
    copiedInProcess_ = 0;
    failedInProcess_ = 0;
    try {
        // 先 refresh：冻结后仍在 refresh 缓冲里的已确认写入必须纳入对账
        client_.refreshIndex(source);
        client_.refreshIndex(target);
        reconcilePass(client_, source, target,
                      copiedInProcess_, failedInProcess_);
        client_.refreshIndex(source);
        client_.refreshIndex(target);
    } catch (const std::exception& e) {
        // 对账异常：解除冻结，保留旧索引服务
        client_.setIndexWriteBlock(source, false);
        writeStage(target, MigrationStage::FAILED,
                   std::string("最终同步失败: ") + e.what());
        return buildStatus(discover());
    }

    // 3) 校验
    VerificationReport report = verify(state);
    if (!report.ok) {
        // 校验失败：解除冻结、旧索引继续服务，记录原因
        client_.setIndexWriteBlock(source, false);
        writeStage(target, MigrationStage::FAILED, report.reason);
        client_.emitLog("校验失败，旧索引继续服务: " + report.reason);
        return buildStatus(discover());
    }

    // 4) 原子别名切换：读、写别名一次性从源移到目标
    writeStage(target, MigrationStage::VERIFIED);
    try {
        pointAliasPairTo(target);
    } catch (const std::exception& e) {
        // 原子操作失败 => 别名未发生任何变化，旧索引仍在服务
        client_.setIndexWriteBlock(source, false);
        writeStage(target, MigrationStage::FAILED,
                   std::string("原子别名切换失败: ") + e.what());
        return buildStatus(discover());
    }

    // 5) 切换完成，解除新索引写阻塞
    client_.setIndexWriteBlock(target, false);

    // 测试钩子：模拟"原子切换成功、但元数据尚未落盘即崩溃"的窗口
    if (std::getenv("ES_MIGRATION_CRASH_AFTER_SWITCH") != nullptr) {
        client_.emitLog("注入崩溃：别名已切到新版本，元数据停留在 VERIFIED");
        std::abort();
    }

    writeStage(target, MigrationStage::SWITCHED);
    client_.emitLog("迁移完成，读写流量已切换至 " + target);
    return buildStatus(discover());
}

// ==================== 回退 ====================

MigrationStatus IndexMigration::rollback() {
    DiscoveredState state = discover();
    if (!state.found) {
        throw ESException("没有可回退的迁移");
    }
    if (state.stage != MigrationStage::SWITCHED &&
        state.stage != MigrationStage::ROLLED_BACK) {
        throw ESException("仅在切换完成（SWITCHED）后才能回退，当前阶段: " +
                          stageToString(state.stage));
    }
    if (state.stage == MigrationStage::ROLLED_BACK) {
        return buildStatus(state);  // 幂等
    }

    const std::string source = state.sourceIndex;
    const std::string target = state.targetIndex;

    // 回退前冻结目标写入，保证不会出现两个可写索引
    client_.setIndexWriteBlock(target, true);

    // 原子地把读、写别名切回旧版本（动作按实时差集生成，可安全重试）
    pointAliasPairTo(source);

    client_.setIndexWriteBlock(source, false);
    writeStage(target, MigrationStage::ROLLED_BACK);
    client_.emitLog("已回退至旧版本 " + source + "，写别名唯一");
    return buildStatus(discover());
}

// ==================== 清理旧版本 ====================

bool IndexMigration::cleanupOldVersion() {
    DiscoveredState state = discover();
    if (!state.found || state.stage != MigrationStage::SWITCHED) {
        throw ESException("只有 SWITCHED 状态、确认新版本服务正常后才能清理旧版本");
    }
    client_.emitLog("清理旧版本索引: " + state.sourceIndex);
    client_.deleteIndex(state.sourceIndex);
    return true;
}

} // namespace es
