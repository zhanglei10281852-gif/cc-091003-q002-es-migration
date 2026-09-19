#ifndef INDEX_MIGRATION_HPP
#define INDEX_MIGRATION_HPP

#include "es_client.hpp"
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>

namespace es {

/**
 * 可恢复的在线索引迁移状态机
 *
 * 别名模型：
 *   <base>-read  读别名：迁移期间指向旧版本，切换后只指向新版本
 *   <base>-write 写别名：任意时刻只指向一个物理索引，保证不存在两个写索引
 *   物理索引名：<base>-v000001、<base>-v000002 ...
 *
 * 状态保存在新版本索引 mapping 的 _meta 中（ES 允许应用自由写 _meta），
 * 进程重启后完全从 Elasticsearch 识别当前阶段：
 *   COPYING      后台连续复制中（旧索引正常读写）
 *   FINAL_SYNC   旧索引已只读冻结，正在做最终一次性对账复制
 *   VERIFIED     校验通过、别名已可切换（防止崩溃窗口的中间态）
 *   SWITCHED     读写流量均已切到新版本（旧索引保留，可回退）
 *   ROLLED_BACK  已回退到旧版本（新版本保留）
 *   FAILED       校验失败，旧索引继续对外服务，附带原因
 */

enum class MigrationStage {
    NOT_STARTED,
    COPYING,
    FINAL_SYNC,
    VERIFIED,
    SWITCHED,
    ROLLED_BACK,
    FAILED
};

struct MigrationProgress {
    int64_t copiedInProcess = 0;  // 本进程已复制的文档操作数
    int64_t sourceCount = 0;      // 源索引文档总数（实时）
    int64_t targetCount = 0;      // 新版本文档总数（实时）
    int64_t failedInProcess = 0;  // 本进程复制失败项
    bool copyThreadAlive = false; // 后台复制线程是否在运行
};

struct MigrationStatus {
    std::string migrationId;
    MigrationStage stage = MigrationStage::NOT_STARTED;
    std::string baseName;
    std::string sourceIndex;
    std::string targetIndex;
    MigrationProgress progress;
    std::string failureReason;
    std::string readAlias;
    std::string writeAlias;
    std::string writeAliasPointsTo;  // 写别名当前实际指向
    std::vector<std::string> readAliasPointsTo;
};

class IndexMigration {
public:
    IndexMigration(ESClient& client,
                   std::string baseName,
                   json newMapping,
                   json newSettings = json::object());

    ~IndexMigration();

    // ==================== 迁移控制 ====================

    /**
     * 发起或继续迁移：
     *  - 从 ES 识别已有阶段（含进程重启后的恢复），绝不重复创建版本
     *  - 首次调用创建下一版本索引、确保别名挂在源索引上，并启动后台复制
     */
    MigrationStatus start();

    /**
     * 等待后台首轮全量复制完成（到达稳定对账状态）
     */
    MigrationStatus waitForCopySettled(int pollIntervalMs = 500,
                                       int timeoutMs = 120000);

    /**
     * 校验（mapping、文档数量、复制失败项）并在通过后执行一次原子别名切换。
     * 校验失败时解除冻结、保留旧索引服务并把原因写入状态。
     * 可在进程重启后重复调用：已切换则幂等返回 SWITCHED。
     */
    MigrationStatus verifyAndSwitch();

    /**
     * 回退：原子地把读写别名切回旧版本，同样保证只有一个写索引。
     * 仅允许在 SWITCHED 阶段（清理旧版本之前）调用，幂等。
     */
    MigrationStatus rollback();

    /**
     * 确认无误后清理旧版本索引；仅在 SWITCHED 阶段允许，此后不可回退。
     */
    bool cleanupOldVersion();

    /**
     * 从 ES 实时重建当前状态
     */
    MigrationStatus getStatus();

    static std::string stageToString(MigrationStage stage);

    void setBatchSize(int size) { batchSize_ = size; }

private:
    ESClient& client_;
    std::string base_;
    json newMapping_;       // 期望的新版本 mappings
    json newSettings_;      // 期望的新版本 settings
    std::string readAlias_;
    std::string writeAlias_;

    int batchSize_ = 200;

    std::thread copyThread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<int64_t> copiedInProcess_{0};
    std::atomic<int64_t> failedInProcess_{0};
    std::mutex metaMutex_;             // 保护 _meta 读改写
    std::mutex threadMutex_;

    // ==================== 状态发现与持久化 ====================
    struct DiscoveredState {
        bool found = false;
        std::string migrationId;
        MigrationStage stage = MigrationStage::NOT_STARTED;
        std::string sourceIndex;
        std::string targetIndex;
        std::string failureReason;
        json meta;
    };

    DiscoveredState discover();
    json readMeta(const std::string& index);
    void writeMeta(const std::string& index, const json& meta);
    void writeStage(const std::string& targetIndex, MigrationStage stage,
                    const std::string& reason = "");
    MigrationStatus buildStatus(const DiscoveredState& state);

    std::vector<std::string> listVersionIndices();
    static int versionOf(const std::string& indexName, const std::string& base);
    std::string versionedName(int version) const;
    static std::string generateMigrationId();

    // ==================== 复制 ====================
    // 以下函数均显式接收 ESClient&：后台复制线程必须传入自己 fork 出的
    // 独立客户端，libcurl handle 不能跨线程共享
    void copyLoop(std::string source, std::string target);
    using BatchHandler = std::function<void(const std::vector<SearchHit>&)>;
    void scrollAll(ESClient& cli, const std::string& index,
                   bool includeSource, const BatchHandler& handler);
    int bulkCopyBatch(ESClient& cli, const std::string& target,
                      const std::vector<SearchHit>& hits,
                      std::atomic<int64_t>& copied,
                      std::atomic<int64_t>& failed);
    struct ReconcileStats {
        int64_t indexed = 0;
        int64_t deleted = 0;
        int64_t failed = 0;
    };
    ReconcileStats reconcilePass(ESClient& cli,
                                 const std::string& source,
                                 const std::string& target,
                                 std::atomic<int64_t>& copiedCounter,
                                 std::atomic<int64_t>& failedCounter);

    // ==================== 校验与切换 ====================
    struct VerificationReport {
        bool ok = false;
        std::string reason;
    };
    VerificationReport verify(const DiscoveredState& state);

    // 原子地把读、写别名的指向统一设置为 index（写别名可写），动作按实时
    // 关联差集生成，因此在任意崩溃中间态下重试都是幂等的
    void pointAliasPairTo(const std::string& index);

    void startCopyThread(const std::string& source, const std::string& target);
    void stopCopyThread();
};

} // namespace es

#endif // INDEX_MIGRATION_HPP
