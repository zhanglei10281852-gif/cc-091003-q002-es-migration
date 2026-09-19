#ifndef ES_CLIENT_HPP
#define ES_CLIENT_HPP

#include "http_client.hpp"
#include "json.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace es {

using json = nlohmann::json;

/**
 * 搜索命中结果
 */
struct SearchHit {
    std::string id;
    std::string index;
    double score;
    json source;
    json highlight;
};

/**
 * 搜索结果
 */
struct SearchResult {
    int total;
    double maxScore;
    std::vector<SearchHit> hits;
    int took;  // 耗时（毫秒）
    bool timedOut;
};

/**
 * 文档操作结果
 */
struct DocResult {
    std::string id;
    std::string index;
    std::string result;  // created, updated, deleted
    int version;
    bool success;
};

/**
 * 批量操作结果
 */
struct BulkResult {
    int took;
    bool errors;
    std::vector<DocResult> items;
    int successCount;
    int failCount;
};

/**
 * Elasticsearch 客户端异常
 */
class ESException : public std::runtime_error {
public:
    explicit ESException(const std::string& message) 
        : std::runtime_error(message) {}
};

/**
 * Elasticsearch 客户端类
 */
class ESClient {
public:
    /**
     * 构造函数
     * @param host ES 主机地址
     * @param port ES 端口
     */
    explicit ESClient(const std::string& host = "localhost", int port = 9200);
    ~ESClient();
    
    // ==================== 集群操作 ====================
    
    /**
     * 检查 ES 连接是否正常
     */
    bool ping();
    
    /**
     * 获取集群健康状态
     */
    json clusterHealth();
    
    /**
     * 获取集群信息
     */
    json clusterInfo();
    
    // ==================== 索引操作 ====================
    
    /**
     * 创建索引
     * @param indexName 索引名称
     * @param mappings 映射配置（可选）
     * @param settings 索引设置（可选）
     */
    bool createIndex(const std::string& indexName,
                     const json& mappings = json::object(),
                     const json& settings = json::object());
    
    /**
     * 删除索引
     */
    bool deleteIndex(const std::string& indexName);
    
    /**
     * 检查索引是否存在
     */
    bool indexExists(const std::string& indexName);
    
    /**
     * 获取索引信息
     */
    json getIndex(const std::string& indexName);
    
    /**
     * 刷新索引（使文档可搜索）
     */
    bool refreshIndex(const std::string& indexName);

    /**
     * 列出匹配模式的索引名（如 "articles_v*"）
     */
    std::vector<std::string> listIndices(const std::string& pattern);

    /**
     * 统计文档数量（索引或别名）
     */
    long countDocuments(const std::string& indexName);

    // ==================== 别名操作 ====================

    /**
     * 检查别名是否存在
     */
    bool aliasExists(const std::string& aliasName);

    /**
     * 获取别名指向的索引列表（别名不存在时返回空列表）
     */
    std::vector<std::string> getAliasIndices(const std::string& aliasName);

    /**
     * 获取索引上的别名列表
     */
    std::vector<std::string> getIndexAliases(const std::string& indexName);

    /**
     * 原子执行一组别名操作（POST /_aliases）
     * 一次调用中的所有 add/remove 动作在 ES 侧原子生效，
     * 用于迁移切换与回退时让读写流量共同转向，避免出现两个写索引。
     * @param actions 动作数组，如 [{"add":{"index":"...","alias":"..."}}, ...]
     */
    bool updateAliases(const json& actions);

    // ==================== 异步复制（Reindex） ====================

    /**
     * 发起异步 reindex（wait_for_completion=false），返回任务 ID
     * @param opType 目标写入类型，"create" 表示不覆盖已有文档（配合 conflicts=proceed 可安全重放）
     * @param conflicts 冲突处理策略，"proceed" 表示跳过冲突继续
     */
    std::string reindexAsync(const std::string& sourceIndex,
                             const std::string& destIndex,
                             const std::string& opType = "create",
                             const std::string& conflicts = "proceed");

    /**
     * 查询任务状态（GET /_tasks/{taskId}）
     * @return 任务信息；任务不存在（如 ES 重启后丢失）返回 std::nullopt
     */
    std::optional<json> getTask(const std::string& taskId);

    // ==================== 双写镜像 ====================

    /**
     * 开启双写镜像：此后经指定写别名的增/删/改/批量操作，
     * 在写入主索引成功后，会同步镜像到 mirrorIndex（物理索引名）。
     * 迁移复制期间用于保证新索引不丢失经写别名到达的写入。
     */
    void setWriteMirror(const std::string& aliasName, const std::string& mirrorIndex);

    /**
     * 关闭双写镜像
     */
    void clearWriteMirror();

    /**
     * 是否开启了双写镜像
     */
    bool hasWriteMirror() const;

    /**
     * 当前镜像目标索引（未开启返回空串）
     */
    std::string writeMirrorIndex() const;

    // ==================== 文档操作 ====================

    /**
     * 索引文档（添加或更新）
     * @param indexName 索引名称
     * @param doc 文档内容
     * @param id 文档 ID（可选，不指定则自动生成）
     * @param refresh 写入后立即刷新（迁移状态等需要立刻可读的文档使用）
     */
    DocResult indexDocument(const std::string& indexName,
                            const json& doc,
                            const std::string& id = "",
                            bool refresh = false);
    
    /**
     * 获取文档
     */
    std::optional<json> getDocument(const std::string& indexName,
                                    const std::string& id);
    
    /**
     * 更新文档
     */
    DocResult updateDocument(const std::string& indexName,
                             const std::string& id,
                             const json& doc);
    
    /**
     * 删除文档
     */
    bool deleteDocument(const std::string& indexName,
                        const std::string& id);
    
    /**
     * 批量索引文档
     */
    BulkResult bulkIndex(const std::string& indexName,
                         const std::vector<json>& docs,
                         const std::vector<std::string>& ids = {});
    
    // ==================== 搜索操作 ====================
    
    /**
     * Match 查询（分词匹配）
     */
    SearchResult matchSearch(const std::string& indexName,
                             const std::string& field,
                             const std::string& query,
                             int from = 0,
                             int size = 10);
    
    /**
     * Multi-Match 查询（多字段匹配）
     */
    SearchResult multiMatchSearch(const std::string& indexName,
                                  const std::vector<std::string>& fields,
                                  const std::string& query,
                                  int from = 0,
                                  int size = 10);
    
    /**
     * Term 查询（精确匹配）
     */
    SearchResult termSearch(const std::string& indexName,
                            const std::string& field,
                            const std::string& value,
                            int from = 0,
                            int size = 10);
    
    /**
     * Bool 组合查询
     */
    SearchResult boolSearch(const std::string& indexName,
                            const json& must = json::array(),
                            const json& should = json::array(),
                            const json& mustNot = json::array(),
                            const json& filter = json::array(),
                            int from = 0,
                            int size = 10);
    
    /**
     * 带高亮的搜索
     */
    SearchResult searchWithHighlight(const std::string& indexName,
                                     const json& query,
                                     const std::vector<std::string>& highlightFields,
                                     int from = 0,
                                     int size = 10);
    
    /**
     * 通用搜索（自定义查询体）
     */
    SearchResult search(const std::string& indexName,
                        const json& queryBody);
    
    // ==================== 日志回调 ====================
    
    using LogCallback = std::function<void(const std::string&)>;
    
    /**
     * 设置日志回调
     */
    void setLogCallback(LogCallback callback);

private:
    std::string baseUrl_;
    HttpClient httpClient_;
    LogCallback logCallback_;

    // 双写镜像配置：mirrorAlias_ 为空表示未开启
    std::string mirrorAlias_;   // 触发镜像的写别名
    std::string mirrorIndex_;   // 镜像写入的物理索引

    void log(const std::string& message);
    std::string buildUrl(const std::string& path);
    SearchResult parseSearchResponse(const json& response);

    /**
     * 判断一次写入是否需要镜像（仅当写入目标是写别名时触发，
     * 直接写物理索引 —— 如镜像自身、迁移状态索引 —— 不会递归镜像）
     */
    bool shouldMirror(const std::string& indexName) const;
};

} // namespace es

#endif // ES_CLIENT_HPP
