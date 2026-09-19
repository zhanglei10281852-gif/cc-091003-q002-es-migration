#include "migration_manager.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <ctime>
#include <algorithm>

namespace es {

namespace {

// 迁移状态持久化索引（单文档，ID 为索引基名）
const char* STATE_INDEX = "es_demo_migration_state";

std::string nowIso() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string nowCompact() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

// 期望映射是否为实际映射的子集：逐字段核对类型、分词器等配置
bool jsonSubset(const json& expected, const json& actual) {
    if (expected.is_object()) {
        if (!actual.is_object()) {
            return false;
        }
        for (auto it = expected.begin(); it != expected.end(); ++it) {
            if (!actual.contains(it.key())) {
                return false;
            }
            if (!jsonSubset(it.value(), actual[it.key()])) {
                return false;
            }
        }
        return true;
    }
    return expected == actual;
}

} // namespace

// ==================== 阶段名称 ====================

std::string migrationPhaseName(MigrationPhase phase) {
    switch (phase) {
        case MigrationPhase::NONE:            return "none";
        case MigrationPhase::PREPARE:         return "prepare";
        case MigrationPhase::COPYING:         return "copying";
        case MigrationPhase::VERIFYING:       return "verifying";
        case MigrationPhase::READY_TO_SWITCH: return "ready_to_switch";
        case MigrationPhase::SWITCHED:        return "switched";
        case MigrationPhase::ABORTED:         return "aborted";
        case MigrationPhase::ROLLED_BACK:     return "rolled_back";
        case MigrationPhase::CLEANED_UP:      return "cleaned_up";
    }
    return "unknown";
}

std::string migrationPhaseDescription(MigrationPhase phase) {
    switch (phase) {
        case MigrationPhase::NONE:            return "无迁移记录";
        case MigrationPhase::PREPARE:         return "已登记，准备创建目标版本";
        case MigrationPhase::COPYING:         return "异步复制旧数据中（双写已开启）";
        case MigrationPhase::VERIFYING:       return "复制完成，校验中";
        case MigrationPhase::READY_TO_SWITCH: return "校验通过，等待原子切换";
        case MigrationPhase::SWITCHED:        return "已切换到新版本（可回退/可清理）";
        case MigrationPhase::ABORTED:         return "校验失败已中止（旧索引继续服务）";
        case MigrationPhase::ROLLED_BACK:     return "已回退到旧版本";
        case MigrationPhase::CLEANED_UP:      return "旧版本已清理（迁移完成）";
    }
    return "未知";
}

// ==================== 构造与日志 ====================

MigrationManager::MigrationManager(ESClient& client, const std::string& baseName)
    : client_(client),
      baseName_(baseName),
      readAlias_(baseName + "_read"),
      writeAlias_(baseName + "_write"),
      stateDocId_(baseName) {
    logCallback_ = [](const std::string& msg) {
        std::cout << msg << "\n";
    };
}

void MigrationManager::setLogCallback(LogCallback callback) {
    logCallback_ = std::move(callback);
}

void MigrationManager::log(const std::string& message) {
    if (logCallback_) {
        logCallback_(message);
    }
}

// ==================== 状态加载与持久化 ====================

bool MigrationManager::loadState() {
    auto doc = client_.getDocument(STATE_INDEX, stateDocId_);
    if (doc) {
        state_ = *doc;
        hasState_ = true;
    } else {
        state_ = json::object();
        hasState_ = false;
    }
    return hasState_;
}

void MigrationManager::saveState() {
    if (!client_.indexExists(STATE_INDEX)) {
        client_.createIndex(STATE_INDEX, json::object(),
                            {{"number_of_shards", 1}, {"number_of_replicas", 0}});
    }
    state_["updated_at"] = nowIso();
    // refresh=true：保证进程重启后能立即读到最新阶段
    client_.indexDocument(STATE_INDEX, state_, stateDocId_, /*refresh=*/true);
}

MigrationPhase MigrationManager::phase() const {
    if (!hasState_) {
        return MigrationPhase::NONE;
    }
    const std::string name = state_.value("phase", "");
    for (auto p : {MigrationPhase::PREPARE, MigrationPhase::COPYING,
                   MigrationPhase::VERIFYING, MigrationPhase::READY_TO_SWITCH,
                   MigrationPhase::SWITCHED, MigrationPhase::ABORTED,
                   MigrationPhase::ROLLED_BACK, MigrationPhase::CLEANED_UP}) {
        if (migrationPhaseName(p) == name) {
            return p;
        }
    }
    return MigrationPhase::NONE;
}

std::string MigrationManager::migrationId() const {
    return state_.value("migration_id", "");
}

void MigrationManager::appendHistory(const std::string& note) {
    if (!state_.contains("history") || !state_["history"].is_array()) {
        state_["history"] = json::array();
    }
    state_["history"].push_back({
        {"phase", state_.value("phase", "")},
        {"at", nowIso()},
        {"note", note}
    });
}

void MigrationManager::transitionTo(MigrationPhase newPhase, const std::string& note) {
    state_["phase"] = migrationPhaseName(newPhase);
    appendHistory(note);
    saveState();
    log("阶段推进 -> " + migrationPhaseName(newPhase) +
        (note.empty() ? "" : "（" + note + "）"));

    // 演示钩子：在指定阶段持久化后模拟进程中断，用于验证断点续跑
    const char* crashAfter = std::getenv("MIGRATE_CRASH_AFTER");
    if (crashAfter && migrationPhaseName(newPhase) == crashAfter) {
        log("【演示】模拟进程中断（MIGRATE_CRASH_AFTER=" +
            std::string(crashAfter) + "），进程退出");
        std::exit(2);
    }
}

// ==================== 工具 ====================

int MigrationManager::parseVersion(const std::string& indexName) const {
    const std::string prefix = baseName_ + "_v";
    if (indexName.rfind(prefix, 0) != 0) {
        return -1;
    }
    const std::string num = indexName.substr(prefix.size());
    if (num.empty() ||
        !std::all_of(num.begin(), num.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return -1;
    }
    return std::stoi(num);
}

std::string MigrationManager::indexNameFor(int version) const {
    return baseName_ + "_v" + std::to_string(version);
}

bool MigrationManager::mappingMatches(const std::string& index, const json& expectedMappings) {
    auto info = client_.getIndex(index);
    if (!info.contains(index)) {
        return false;
    }
    const json actual = info[index].value("mappings", json::object());
    return jsonSubset(expectedMappings, actual);
}

std::set<std::string> MigrationManager::allDocIds(const std::string& index) {
    // 演示规模一次取回；生产环境应改用 scroll / search_after 分批对比
    json body = {
        {"size", 10000},
        {"_source", false},
        {"query", {{"match_all", json::object()}}}
    };
    auto result = client_.search(index, body);
    if (result.total > 10000) {
        throw ESException("文档数超过 10000，超出演示核对范围（生产请改用 scroll 分批对比）");
    }
    std::set<std::string> ids;
    for (const auto& hit : result.hits) {
        ids.insert(hit.id);
    }
    return ids;
}

void MigrationManager::reconcileDeletes(const std::string& source, const std::string& target) {
    // 复制基于快照：复制期间经写别名删除的文档，可能仍被快照带入目标索引。
    // 对齐两侧文档 ID，清理目标索引中多出的残留（"僵尸文档"）。
    const auto sourceIds = allDocIds(source);
    const auto targetIds = allDocIds(target);
    int removed = 0;
    for (const auto& id : targetIds) {
        if (sourceIds.find(id) == sourceIds.end()) {
            client_.deleteDocument(target, id);  // 直接写物理索引，不触发镜像
            removed++;
        }
    }
    if (removed > 0) {
        client_.refreshIndex(target);
    }
    log("删除对齐: 目标索引清理残留文档 " + std::to_string(removed) + " 篇");
}

// ==================== 迁移登记 ====================

void MigrationManager::startNew(const json& newMappings, const json& newSettings) {
    // 当前写索引 = 写别名指向的唯一索引
    const auto writeTargets = client_.getAliasIndices(writeAlias_);
    if (writeTargets.empty()) {
        throw ESException("写别名 " + writeAlias_ + " 不存在，请先运行 bootstrap 初始化");
    }
    if (writeTargets.size() != 1) {
        throw ESException("写别名 " + writeAlias_ + " 指向多个索引，状态异常，请人工核查");
    }
    const std::string source = writeTargets[0];
    const int version = parseVersion(source);
    if (version < 0) {
        throw ESException("无法从索引名解析版本号: " + source);
    }
    const std::string target = indexNameFor(version + 1);

    // 目标版本已存在但没有迁移记录：属于遗留半成品，确认未接流量后清理重建
    if (client_.indexExists(target)) {
        if (!client_.getIndexAliases(target).empty()) {
            throw ESException("目标版本 " + target + " 已被别名占用，请人工核查");
        }
        log("发现无迁移状态的遗留索引 " + target + "，清理后重建");
        client_.deleteIndex(target);
    }

    state_ = json::object();
    state_["migration_id"] = "mig_" + source + "_to_" + target + "_" + nowCompact();
    state_["base"] = baseName_;
    state_["read_alias"] = readAlias_;
    state_["write_alias"] = writeAlias_;
    state_["source_index"] = source;
    state_["target_index"] = target;
    state_["new_mappings"] = newMappings;
    state_["new_settings"] = newSettings;
    state_["created_at"] = nowIso();
    state_["reindex"] = {
        {"task_id", ""},
        {"created", 0},
        {"version_conflicts", 0},
        {"failures", json::array()}
    };
    state_["verify"] = json::object();
    state_["phase"] = migrationPhaseName(MigrationPhase::PREPARE);
    state_["history"] = json::array();
    hasState_ = true;
    appendHistory("登记迁移 " + state_["migration_id"].get<std::string>() +
                  "（" + source + " -> " + target + "）");
    saveState();
    log("发起迁移 " + state_["migration_id"].get<std::string>());
}

// ==================== 阶段实现 ====================

bool MigrationManager::armCopy() {
    // 双写镜像：复制期间经写别名到达的增/删/改同步到新索引
    client_.setWriteMirror(writeAlias_, state_["target_index"].get<std::string>());

    // 异步复制任务（已启动则复用，不重复发起）
    if (state_["reindex"].value("task_id", "").empty()) {
        const std::string taskId = client_.reindexAsync(
            state_["source_index"].get<std::string>(),
            state_["target_index"].get<std::string>(),
            "create", "proceed");
        state_["reindex"]["task_id"] = taskId;
        appendHistory("异步复制启动 task=" + taskId);
        saveState();
        log("异步复制已启动，任务 " + taskId);
        return true;
    }
    return false;
}

void MigrationManager::doPrepare() {
    const std::string target = state_["target_index"].get<std::string>();
    const json expected = state_["new_mappings"];
    const json settings = state_.value("new_settings", json::object());

    if (!client_.indexExists(target)) {
        client_.createIndex(target, expected, settings);
        log("创建目标索引 " + target);
    } else if (!mappingMatches(target, expected)) {
        // 半成品索引映射与预期不符：未接流量，安全重建
        log("目标索引已存在但映射与预期不符（半成品），重建 " + target);
        client_.deleteIndex(target);
        client_.createIndex(target, expected, settings);
    } else {
        log("目标索引已存在（断点恢复），跳过创建: " + target);
    }

    if (phase() == MigrationPhase::PREPARE) {
        transitionTo(MigrationPhase::COPYING, "目标索引 " + target + " 就绪");
    }
    // 注：双写镜像与复制任务在 doCopy 的 armCopy 中启动，
    // 保证崩溃点 copying（未启动复制）与 reindex（复制已启动）可区分
}

void MigrationManager::doCopy() {
    const bool taskJustStarted = armCopy();  // 断点恢复：确保镜像与复制任务就绪

    // 演示钩子：复制任务启动后中断（任务 id 已持久化，用于验证任务丢失恢复）
    if (taskJustStarted) {
        const char* crashAfter = std::getenv("MIGRATE_CRASH_AFTER");
        if (crashAfter && std::string(crashAfter) == "reindex") {
            log("【演示】模拟进程中断（MIGRATE_CRASH_AFTER=reindex），进程退出");
            std::exit(2);
        }
    }

    const std::string source = state_["source_index"].get<std::string>();
    const std::string target = state_["target_index"].get<std::string>();
    std::string taskId = state_["reindex"].value("task_id", "");

    while (true) {
        auto task = client_.getTask(taskId);
        if (!task) {
            // 任务在 ES 中不存在（如 ES 重启丢失）：create-only 复制可安全重发
            log("复制任务 " + taskId + " 在 ES 中不存在（可能 ES 重启），重新发起复制");
            taskId = client_.reindexAsync(source, target, "create", "proceed");
            state_["reindex"]["task_id"] = taskId;
            appendHistory("复制任务丢失，重新发起 task=" + taskId);
            saveState();
            continue;
        }
        if (!task->value("completed", false)) {
            const long created = task->value(json::json_pointer("/task/status/created"), 0L);
            const long total = task->value(json::json_pointer("/task/status/total"), 0L);
            log("复制进度: " + std::to_string(created) + "/" + std::to_string(total));
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        const json resp = task->value("response", json::object());
        state_["reindex"]["created"] = resp.value("created", 0);
        state_["reindex"]["updated"] = resp.value("updated", 0);
        state_["reindex"]["noops"] = resp.value("noops", 0);
        state_["reindex"]["total"] = resp.value("total", 0);
        state_["reindex"]["version_conflicts"] = resp.value("version_conflicts", 0);
        state_["reindex"]["failures"] = resp.value("failures", json::array());
        saveState();

        const auto failures = state_["reindex"]["failures"].size();
        log("复制完成: 新增 " + std::to_string(state_["reindex"]["created"].get<long>()) +
            "，冲突跳过 " + std::to_string(state_["reindex"]["version_conflicts"].get<long>()) +
            "（双写文档按预期保留），失败 " + std::to_string(failures));
        break;
    }

    transitionTo(MigrationPhase::VERIFYING, "异步复制完成");
}

bool MigrationManager::doVerify() {
    const std::string source = state_["source_index"].get<std::string>();
    const std::string target = state_["target_index"].get<std::string>();

    client_.refreshIndex(source);
    client_.refreshIndex(target);

    // 删除对齐：清理复制快照带入目标索引的残留文档
    reconcileDeletes(source, target);

    // 核对一：目标索引映射与预期一致
    const bool mappingOk = mappingMatches(target, state_["new_mappings"]);

    // 核对二：源/目标文档数量一致
    const long sourceCount = client_.countDocuments(source);
    const long targetCount = client_.countDocuments(target);
    const bool countOk = (sourceCount == targetCount);

    // 核对三：复制失败项为零
    const long reindexFailures = static_cast<long>(
        state_.value("reindex", json::object()).value("failures", json::array()).size());
    const bool failuresOk = (reindexFailures == 0);

    // 演示钩子：强制校验失败，用于观察中止路径
    std::string injected;
    const char* failEnv = std::getenv("MIGRATE_DEMO_FAIL_VERIFY");
    if (failEnv && *failEnv) {
        injected = std::string("演示注入的校验失败（MIGRATE_DEMO_FAIL_VERIFY=") + failEnv + "）";
    }

    const bool ok = mappingOk && countOk && failuresOk && injected.empty();

    std::string reason;
    if (!mappingOk) {
        reason += "目标索引映射与预期不符; ";
    }
    if (!countOk) {
        reason += "文档数量不一致（源=" + std::to_string(sourceCount) +
                  "，目标=" + std::to_string(targetCount) + "）; ";
    }
    if (!failuresOk) {
        reason += "复制失败项 " + std::to_string(reindexFailures) + " 个; ";
    }
    if (!injected.empty()) {
        reason += injected;
    }

    state_["verify"] = {
        {"mapping_ok", mappingOk},
        {"source_count", sourceCount},
        {"target_count", targetCount},
        {"count_ok", countOk},
        {"reindex_failures", reindexFailures},
        {"failures_ok", failuresOk},
        {"ok", ok},
        {"reason", reason},
        {"checked_at", nowIso()}
    };
    saveState();

    if (!ok) {
        transitionTo(MigrationPhase::ABORTED, "校验失败: " + reason);
        client_.clearWriteMirror();
        log("校验未通过，保持旧索引服务，读写流量不切换。原因: " + reason);
        return false;
    }

    transitionTo(MigrationPhase::READY_TO_SWITCH, "校验通过（映射/文档数量/复制失败项）");
    return true;
}

void MigrationManager::doSwitch() {
    const std::string source = state_["source_index"].get<std::string>();
    const std::string target = state_["target_index"].get<std::string>();

    // 根据别名实际指向计算所需动作（幂等：已切换则无需动作）
    json actions = json::array();
    for (const auto& alias : {readAlias_, writeAlias_}) {
        const auto current = client_.getAliasIndices(alias);
        for (const auto& idx : current) {
            if (idx != target) {
                actions.push_back({{"remove", {{"index", idx}, {"alias", alias}}}});
            }
        }
        if (std::find(current.begin(), current.end(), target) == current.end()) {
            actions.push_back({{"add", {{"index", target}, {"alias", alias}}}});
        }
    }

    if (actions.empty()) {
        log("别名已指向目标版本（断点恢复），跳过切换");
    } else {
        // 一次原子别名操作：读写流量共同转向新版本
        client_.updateAliases(actions);
        log("原子切换完成: " + readAlias_ + " / " + writeAlias_ + " -> " + target);
    }

    transitionTo(MigrationPhase::SWITCHED, "读写流量已切换到 " + target);

    // 反向镜像：清理旧版本前，新写入同步回旧索引，保证回退不丢数据
    client_.setWriteMirror(writeAlias_, source);
}

void MigrationManager::doRollback() {
    const std::string source = state_["source_index"].get<std::string>();
    const std::string target = state_["target_index"].get<std::string>();

    // 一次原子别名操作切回旧版本：写别名始终只指向一个索引
    json actions = json::array();
    for (const auto& alias : {readAlias_, writeAlias_}) {
        const auto current = client_.getAliasIndices(alias);
        for (const auto& idx : current) {
            if (idx != source) {
                actions.push_back({{"remove", {{"index", idx}, {"alias", alias}}}});
            }
        }
        if (std::find(current.begin(), current.end(), source) == current.end()) {
            actions.push_back({{"add", {{"index", source}, {"alias", alias}}}});
        }
    }

    if (actions.empty()) {
        log("别名已指向旧版本，无需回退动作");
    } else {
        client_.updateAliases(actions);
        log("原子回退完成: " + readAlias_ + " / " + writeAlias_ + " -> " + source);
    }

    transitionTo(MigrationPhase::ROLLED_BACK, "读写流量已回退到 " + source);

    // 恢复正向镜像：目标索引保持同步，可重新校验后再次切换
    client_.setWriteMirror(writeAlias_, target);
}

// ==================== 分阶段接口 ====================

bool MigrationManager::prepareStep(const json& newMappings, const json& newSettings) {
    loadState();
    if (!hasState_ || phase() == MigrationPhase::CLEANED_UP) {
        startNew(newMappings, newSettings);
    }
    if (phase() == MigrationPhase::PREPARE) {
        doPrepare();  // 创建/核对目标索引，进入 COPYING
    }
    if (phase() == MigrationPhase::COPYING) {
        armCopy();    // 开启双写镜像并启动异步复制（幂等）
        return true;
    }
    log("当前阶段为 " + migrationPhaseName(phase()) + "，无需准备");
    return true;
}

bool MigrationManager::copyStep() {
    loadState();
    if (phase() == MigrationPhase::COPYING) {
        doCopy();
        return true;
    }
    log("当前阶段为 " + migrationPhaseName(phase()) + "，无需复制");
    return true;
}

bool MigrationManager::verifyStep() {
    loadState();
    const auto p = phase();
    if (p == MigrationPhase::VERIFYING || p == MigrationPhase::ABORTED) {
        return doVerify();
    }
    if (p == MigrationPhase::READY_TO_SWITCH || p == MigrationPhase::SWITCHED) {
        log("校验已通过，无需重复校验");
        return true;
    }
    log("当前阶段为 " + migrationPhaseName(p) + "，无法校验");
    return false;
}

bool MigrationManager::switchStep() {
    loadState();
    const auto p = phase();
    if (p == MigrationPhase::READY_TO_SWITCH) {
        doSwitch();
        return true;
    }
    if (p == MigrationPhase::SWITCHED) {
        log("已处于切换完成状态");
        return true;
    }
    log("当前阶段为 " + migrationPhaseName(p) + "，校验未通过，不能切换");
    return false;
}

bool MigrationManager::rollbackStep() {
    loadState();
    const auto p = phase();
    if (p == MigrationPhase::ROLLED_BACK) {
        log("已处于回退状态");
        return true;
    }
    if (p != MigrationPhase::SWITCHED) {
        log("当前阶段为 " + migrationPhaseName(p) + "，仅切换后、清理前可回退");
        return false;
    }
    doRollback();
    return true;
}

bool MigrationManager::cleanupStep() {
    loadState();
    if (!hasState_) {
        log("无迁移记录，无需清理");
        return true;
    }
    const auto p = phase();
    if (p == MigrationPhase::SWITCHED) {
        // 切换完成：删除旧版本，迁移收尾
        client_.clearWriteMirror();
        const std::string source = state_["source_index"].get<std::string>();
        if (client_.indexExists(source)) {
            client_.deleteIndex(source);
        }
        transitionTo(MigrationPhase::CLEANED_UP, "旧版本 " + source + " 已删除，迁移完成");
        return true;
    }
    if (p == MigrationPhase::ABORTED || p == MigrationPhase::ROLLED_BACK) {
        // 中止/回退：删除未启用的目标版本，清除迁移记录
        client_.clearWriteMirror();
        const std::string target = state_["target_index"].get<std::string>();
        if (client_.indexExists(target)) {
            client_.deleteIndex(target);
            log("已删除未启用的目标索引 " + target);
        }
        client_.deleteDocument(STATE_INDEX, stateDocId_);
        client_.refreshIndex(STATE_INDEX);
        hasState_ = false;
        state_ = json::object();
        log("迁移记录已清除");
        return true;
    }
    log("当前阶段为 " + migrationPhaseName(p) + "，不可清理");
    return false;
}

// ==================== 一键流程 ====================

bool MigrationManager::run(const json& newMappings, const json& newSettings) {
    loadState();
    if (!hasState_ || phase() == MigrationPhase::CLEANED_UP) {
        startNew(newMappings, newSettings);
    } else {
        log("检测到进行中的迁移 " + migrationId() +
            "（阶段: " + migrationPhaseName(phase()) + "），从断点继续");
    }

    while (true) {
        switch (phase()) {
            case MigrationPhase::PREPARE:
                doPrepare();
                break;
            case MigrationPhase::COPYING:
                doCopy();
                break;
            case MigrationPhase::VERIFYING:
                if (!doVerify()) {
                    return false;
                }
                break;
            case MigrationPhase::READY_TO_SWITCH:
                doSwitch();
                break;
            case MigrationPhase::SWITCHED:
                log("迁移已切换，读写流量在新版本 " +
                    state_["target_index"].get<std::string>() +
                    "。可执行 rollback 回退或 cleanup 清理旧版本。");
                return true;
            case MigrationPhase::ROLLED_BACK:
                transitionTo(MigrationPhase::VERIFYING, "回退后重新校验");
                break;
            case MigrationPhase::ABORTED:
                log("迁移已中止: " + state_["verify"].value("reason", "") +
                    "。旧索引继续服务。处理后可执行 cleanup 再重新 migrate。");
                return false;
            case MigrationPhase::CLEANED_UP:
                return true;
            case MigrationPhase::NONE:
                throw ESException("迁移状态异常：无记录");
        }
    }
}

// ==================== 双写镜像 ====================

void MigrationManager::attachWriteMirror() {
    loadState();
    if (!hasState_) {
        client_.clearWriteMirror();
        return;
    }
    switch (phase()) {
        case MigrationPhase::COPYING:
        case MigrationPhase::VERIFYING:
        case MigrationPhase::READY_TO_SWITCH:
        case MigrationPhase::ROLLED_BACK:
            client_.setWriteMirror(writeAlias_, state_["target_index"].get<std::string>());
            break;
        case MigrationPhase::SWITCHED:
            client_.setWriteMirror(writeAlias_, state_["source_index"].get<std::string>());
            break;
        default:
            client_.clearWriteMirror();
            break;
    }
}

void MigrationManager::attachWriteMirrorFor(ESClient& client, const std::string& baseName) {
    MigrationManager manager(client, baseName);
    manager.attachWriteMirror();
}

// ==================== 状态输出 ====================

void MigrationManager::printStatus() {
    loadState();

    log("── 迁移状态 ──────────────────────────────");
    if (!hasState_) {
        log("  当前无进行中的迁移");
    } else {
        log("  迁移标识: " + migrationId());
        log("  当前阶段: " + migrationPhaseName(phase()) +
            "（" + migrationPhaseDescription(phase()) + "）");
        log("  源索引:   " + state_.value("source_index", ""));
        log("  目标索引: " + state_.value("target_index", ""));

        const json reindex = state_.value("reindex", json::object());
        const json reindexFailures = reindex.value("failures", json::array());
        log("  复制统计: 新增 " + std::to_string(reindex.value("created", 0)) +
            "，冲突跳过 " + std::to_string(reindex.value("version_conflicts", 0)) +
            "，失败 " + std::to_string(reindexFailures.size()) +
            (reindex.value("task_id", "").empty()
                 ? "" : "（任务 " + reindex.value("task_id", "") + "）"));

        const json verify = state_.value("verify", json::object());
        if (!verify.empty()) {
            std::string line = verify.value("ok", false) ? "  校验结论: 通过（" : "  校验结论: 未通过（";
            line += "映射" + std::string(verify.value("mapping_ok", false) ? "✓" : "✗") +
                    "，文档数 " + std::to_string(verify.value("source_count", 0)) +
                    "=" + std::to_string(verify.value("target_count", 0)) +
                    (verify.value("count_ok", false) ? "✓" : "✗") +
                    "，复制失败项 " + std::to_string(verify.value("reindex_failures", 0)) +
                    (verify.value("failures_ok", false) ? "✓" : "✗") + "）";
            log(line);
            const std::string reason = verify.value("reason", "");
            if (!reason.empty()) {
                log("  失败原因: " + reason);
            }
        }

        const json history = state_.value("history", json::array());
        if (!history.empty()) {
            log("  阶段进度:");
            for (const auto& h : history) {
                log("    [" + h.value("at", "") + "] " +
                    h.value("phase", "") + "  " + h.value("note", ""));
            }
        }
    }

    log("── 别名指向 ──────────────────────────────");
    for (const auto& alias : {readAlias_, writeAlias_}) {
        const auto targets = client_.getAliasIndices(alias);
        std::string line = "  " + alias + " -> ";
        if (targets.empty()) {
            line += "(未指向任何索引)";
        } else {
            for (size_t i = 0; i < targets.size(); ++i) {
                if (i > 0) {
                    line += ", ";
                }
                line += targets[i];
            }
        }
        log(line);
    }

    const auto versions = client_.listIndices(baseName_ + "_v*");
    std::string line = "  版本索引: ";
    if (versions.empty()) {
        line += "(无)";
    } else {
        for (size_t i = 0; i < versions.size(); ++i) {
            if (i > 0) {
                line += ", ";
            }
            line += versions[i];
        }
    }
    log(line);
}

} // namespace es
