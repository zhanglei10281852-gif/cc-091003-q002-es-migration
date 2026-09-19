#ifndef ES_CLIENT_HPP
#define ES_CLIENT_HPP

#include "http_client.hpp"
#include "json.hpp"
#include <string>
#include <vector>
#include <map>
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

    /**
     * 派生一个独立连接的客户端（各自持有 libcurl handle，可在不同线程使用）
     */
    std::unique_ptr<ESClient> fork() const;
    
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
    
    // ==================== 文档操作 ====================
    
    /**
     * 索引文档（添加或更新）
     * @param indexName 索引名称
     * @param doc 文档内容
     * @param id 文档 ID（可选，不指定则自动生成）
     */
    DocResult indexDocument(const std::string& indexName,
                            const json& doc,
                            const std::string& id = "");
    
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

    // ==================== 别名与迁移相关操作 ====================

    /**
     * 批量操作中的单条动作
     * op 取值："index"（写文档）或 "delete"（删文档）
     */
    struct BulkAction {
        std::string op;
        std::string index;
        std::string id;
        json doc;
    };

    /**
     * 混合批量操作（同一批中可包含 index / delete）
     * 用于迁移复制时把旧索引的删除同步到新索引
     */
    BulkResult bulkActions(const std::vector<BulkAction>& actions);

    /**
     * 原子别名操作：一次性提交多组 add/remove，Elasticsearch 保证整体原子生效
     * @param actions 形如 {"actions":[{"add":{...}},{"remove":{...}}]}
     */
    bool updateAliases(const json& actions);

    /**
     * 查询别名指向的物理索引
     * @return alias 名称 -> 物理索引名列表（正常情况下写别名应只有一个元素）
     */
    std::map<std::string, std::vector<std::string>> getAliases(
            const std::string& aliasNames = "");

    /**
     * 解析单个别名当前指向的物理索引；不存在或指向多个索引时返回空串
     */
    std::string resolveAlias(const std::string& aliasName);

    /**
     * 设置/解除索引只读写阻塞（迁移终同步期间冻结写入）
     */
    bool setIndexWriteBlock(const std::string& indexName, bool blocked);

    /**
     * 获取索引文档数量
     */
    long documentCount(const std::string& indexName);

    /**
     * 获取索引 mapping（返回该索引下的 mappings 对象）
     */
    json getMapping(const std::string& indexName);

    /**
     * 透传底层 HTTP 请求，供迁移状态机调用 scroll / _pit / _count 等接口
     * @param method GET / POST / PUT / DELETE
     * @param path 以 / 开头的路径（可带 query string）
     */
    HttpResponse rawRequest(const std::string& method,
                            const std::string& path,
                            const std::string& body = "");
    
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

    /**
     * 输出一条日志（走已设置的日志回调）
     */
    void emitLog(const std::string& message) { log(message); }

private:
    std::string baseUrl_;
    std::string host_;
    int port_ = 9200;
    HttpClient httpClient_;
    LogCallback logCallback_;
    
    void log(const std::string& message);
    std::string buildUrl(const std::string& path);
    SearchResult parseSearchResponse(const json& response);
};

} // namespace es

#endif // ES_CLIENT_HPP
