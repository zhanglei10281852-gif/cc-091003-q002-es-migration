#ifndef MIGRATION_MANAGER_HPP
#define MIGRATION_MANAGER_HPP

#include "es_client.hpp"
#include <string>
#include <functional>
#include <set>

namespace es {

/**
 * 迁移阶段（持久化在 Elasticsearch 的迁移状态文档中）
 *
 * 状态机：
 *   PREPARE -> COPYING -> VERIFYING -> READY_TO_SWITCH -> SWITCHED -> CLEANED_UP
 *                |            |
 *                |            +-> ABORTED（校验失败，旧索引继续服务）
 *             SWITCHED -> ROLLED_BACK（回退）-> VERIFYING（可重新校验后再次切换）
 */
enum class MigrationPhase {
    NONE,            // 无迁移记录
    PREPARE,         // 已登记迁移，目标版本待创建
    COPYING,         // 异步复制旧数据中（双写镜像已开启）
    VERIFYING,       // 复制完成，校验中
    READY_TO_SWITCH, // 校验通过，等待原子切换
    SWITCHED,        // 已切换到新版本（此阶段可回退、可清理）
    ABORTED,         // 校验失败已中止（旧索引继续服务，流量未切换）
    ROLLED_BACK,     // 已回退到旧版本
    CLEANED_UP       // 旧版本已清理（迁移终态）
};

std::string migrationPhaseName(MigrationPhase phase);
std::string migrationPhaseDescription(MigrationPhase phase);

/**
 * 可恢复的索引迁移管理器
 *
 * 职责：
 *  - 迁移状态（迁移标识、阶段、复制任务、校验结论、历史）持久化在 ES 的
 *    es_demo_migration_state 索引中，进程中断后再次执行可从 ES 识别阶段并继续；
 *  - 发起迁移时创建带版本号的新索引（如 articles_v2），异步 reindex 旧数据；
 *  - 复制期间为客户端挂载双写镜像，经写别名到达的增/删/改同步到新索引；
 *  - 校验（映射、文档数量、复制失败项）通过后，一次原子别名操作让读写流量
 *    共同转向新版本；校验失败则保留旧索引服务并记录原因；
 *  - 切换后、清理前可原子回退；回退/切换均保证写别名只指向一个索引。
 */
class MigrationManager {
public:
    /**
     * @param client ES 客户端
     * @param baseName 索引基名（如 "articles"，对应别名 articles_read / articles_write，
     *                 版本索引 articles_v1、articles_v2 ...）
     */
    MigrationManager(ESClient& client, const std::string& baseName);

    using LogCallback = std::function<void(const std::string&)>;
    void setLogCallback(LogCallback callback);

    // ==================== 状态 ====================

    /**
     * 从 ES 加载迁移状态（每个阶段操作前都会调用，保证断点续跑基于最新状态）
     * @return 是否存在进行中的迁移记录
     */
    bool loadState();

    bool hasState() const { return hasState_; }
    MigrationPhase phase() const;
    const json& state() const { return state_; }
    std::string migrationId() const;

    // ==================== 流程入口 ====================

    /**
     * 一键执行：无迁移记录则发起新迁移，有记录则从中断阶段继续，
     * 直到 SWITCHED（返回 true）或 ABORTED（返回 false）。
     */
    bool run(const json& newMappings, const json& newSettings);

    // ==================== 分阶段接口（集成场景 / 运维单步执行） ====================

    bool prepareStep(const json& newMappings, const json& newSettings); // 登记+建目标+启动异步复制
    bool copyStep();        // 等待异步复制完成
    bool verifyStep();      // 校验：映射 / 文档数量 / 复制失败项
    bool switchStep();      // 原子切换读写别名到新版本
    bool rollbackStep();    // 原子回退读写别名到旧版本（仅 SWITCHED 阶段）
    bool cleanupStep();     // SWITCHED 后清理旧版本；ABORTED/ROLLED_BACK 后清理半成品

    // ==================== 双写镜像 ====================

    /**
     * 按 ES 中的迁移状态为当前客户端挂载/摘除双写镜像：
     *  复制期间（COPYING/VERIFYING/READY_TO_SWITCH/ROLLED_BACK）镜像到新索引；
     *  切换后清理前（SWITCHED）反向镜像到旧索引，保证回退不丢数据。
     * 业务进程启动时调用一次即可与迁移流程配合。
     */
    void attachWriteMirror();

    /**
     * 为任意客户端按迁移状态挂载双写镜像（供业务写入方使用）
     */
    static void attachWriteMirrorFor(ESClient& client, const std::string& baseName);

    // ==================== 状态输出 ====================

    /**
     * 打印迁移标识、各阶段进度、校验结论和当前别名指向
     */
    void printStatus();

private:
    ESClient& client_;
    std::string baseName_;
    std::string readAlias_;
    std::string writeAlias_;
    std::string stateDocId_;
    LogCallback logCallback_;

    bool hasState_ = false;
    json state_ = json::object();

    void log(const std::string& message);

    // ---- 状态持久化 ----
    void saveState();
    void appendHistory(const std::string& note);
    void transitionTo(MigrationPhase phase, const std::string& note);

    // ---- 阶段实现（均幂等，可断点续跑） ----
    void startNew(const json& newMappings, const json& newSettings); // 登记新迁移（计算下一版本号）
    void doPrepare();    // 创建/核对目标索引，开启双写，启动异步复制
    void doCopy();       // 轮询复制任务（任务丢失则安全重发），记录复制统计
    bool doVerify();     // 删除对齐 + 三项校验，通过则 READY_TO_SWITCH，否则 ABORTED
    void doSwitch();     // 原子切换读写别名到目标版本
    void doRollback();   // 原子切回旧版本

    // ---- 工具 ----
    bool armCopy();                                   // 确保双写镜像与复制任务就绪；返回是否新启动了复制任务
    bool mappingMatches(const std::string& index, const json& expectedMappings);
    std::set<std::string> allDocIds(const std::string& index);
    void reconcileDeletes(const std::string& source, const std::string& target);
    int parseVersion(const std::string& indexName) const;
    std::string indexNameFor(int version) const;
};

} // namespace es

#endif // MIGRATION_MANAGER_HPP
