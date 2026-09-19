#include "es_client.hpp"
#include "migration.hpp"

#include <iostream>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <map>
#include <mutex>

using namespace es;
using json = nlohmann::json;

// ==================== 控制台输出 ====================

namespace Color {
    const std::string RESET   = "\033[0m";
    const std::string RED     = "\033[31m";
    const std::string GREEN   = "\033[32m";
    const std::string YELLOW  = "\033[33m";
    const std::string BLUE    = "\033[34m";
    const std::string MAGENTA = "\033[35m";
    const std::string CYAN    = "\033[36m";
    const std::string BOLD    = "\033[1m";
}

void printHeader(const std::string& title) {
    std::cout << "\n" << Color::CYAN << Color::BOLD;
    std::cout << "========================================\n";
    std::cout << "  " << title << "\n";
    std::cout << "========================================\n";
    std::cout << Color::RESET;
}

void printSection(const std::string& title) {
    std::cout << "\n" << Color::YELLOW << Color::BOLD;
    std::cout << "--- " << title << " ---\n";
    std::cout << Color::RESET;
}

void printSuccess(const std::string& message) {
    std::cout << Color::GREEN << "✓ " << message << Color::RESET << "\n";
}

void printError(const std::string& message) {
    std::cout << Color::RED << "✗ " << message << Color::RESET << "\n";
}

void printInfo(const std::string& message) {
    std::cout << Color::BLUE << "→ " << message << Color::RESET << "\n";
}

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (condition) {
        printSuccess(message);
    } else {
        printError(message);
        ++g_failures;
    }
}

// ==================== 业务常量 ====================

const std::string BASE       = "articles";
const std::string READ_ALIAS = "articles-read";
const std::string WRITE_ALIAS = "articles-write";

// 旧 mapping（线上现役版本）
json oldMapping() {
    return {
        {"properties", {
            {"title",    {{"type", "text"}, {"analyzer", "standard"},
                          {"fields", {{"keyword", {{"type", "keyword"}}}}}}},
            {"content",  {{"type", "text"}, {"analyzer", "standard"}}},
            {"author",   {{"type", "keyword"}}},
            {"category", {{"type", "keyword"}}},
            {"tags",     {{"type", "keyword"}}},
            {"created_at", {{"type", "date"}, {"format", "yyyy-MM-dd"}}}
        }}
    };
}

// 新 mapping：新增字段类型 summary(text+keyword) 与 word_count(integer)
json newMapping() {
    json m = oldMapping();
    m["properties"]["summary"] = {
        {"type", "text"},
        {"fields", {{"keyword", {{"type", "keyword"}}}}}
    };
    m["properties"]["word_count"] = {{"type", "integer"}};
    return m;
}

json indexSettings() {
    return {{"number_of_shards", 1}, {"number_of_replicas", 0}};
}

std::vector<json> sampleArticles() {
    return {
        {{"title", "人工智能的发展历程"},
         {"content", "人工智能（AI）是计算机科学的一个分支，致力于创建能够执行通常需要人类智能的任务的系统。"},
         {"author", "张三"}, {"category", "技术"},
         {"tags", json::array({"AI", "人工智能", "机器学习"})},
         {"created_at", "2024-01-15"}},
        {{"title", "深度学习入门指南"},
         {"content", "深度学习是机器学习的一个子领域，使用多层神经网络来学习数据的层次化表示。"},
         {"author", "李四"}, {"category", "技术"},
         {"tags", json::array({"深度学习", "神经网络"})},
         {"created_at", "2024-02-20"}},
        {{"title", "Elasticsearch 搜索引擎实战"},
         {"content", "Elasticsearch是一个分布式、RESTful风格的搜索和数据分析引擎。"},
         {"author", "王五"}, {"category", "技术"},
         {"tags", json::array({"Elasticsearch", "搜索引擎", "全文检索"})},
         {"created_at", "2024-03-10"}},
        {{"title", "C++17 新特性详解"},
         {"content", "C++17引入了许多新特性，包括结构化绑定、if constexpr、折叠表达式等。"},
         {"author", "赵六"}, {"category", "编程语言"},
         {"tags", json::array({"C++", "C++17", "编程"})},
         {"created_at", "2024-04-05"}},
        {{"title", "微服务架构设计模式"},
         {"content", "微服务架构是一种将应用程序构建为一组小型服务的方法。"},
         {"author", "钱七"}, {"category", "架构"},
         {"tags", json::array({"微服务", "架构", "分布式"})},
         {"created_at", "2024-05-18"}}
    };
}

std::unique_ptr<ESClient> makeClient() {
    const char* esHost = std::getenv("ES_HOST");
    const char* esPort = std::getenv("ES_PORT");
    std::string host = esHost ? esHost : "127.0.0.1";
    // 容器内走服务名 elasticsearch；本地直连绕过代理
    int port = esPort ? std::stoi(esPort) : 9200;
    auto client = std::make_unique<ESClient>(host, port);
    client->setLogCallback([](const std::string& msg) {
        std::cout << Color::MAGENTA << "    [迁移日志] " << msg
                  << Color::RESET << "\n";
    });
    return client;
}

void waitForEs(ESClient& client) {
    std::cout << "等待 Elasticsearch 就绪";
    int retries = 60;
    while (retries > 0 &&
           client.rawRequest("GET", "/_cluster/health").statusCode != 200) {
        std::cout << "." << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        --retries;
    }
    std::cout << "\n";
    if (retries == 0) {
        throw std::runtime_error("无法连接到 Elasticsearch");
    }
}

void printStatus(const MigrationStatus& s) {
    std::cout << "  迁移标识 : " << Color::BOLD
              << (s.migrationId.empty() ? "(无)" : s.migrationId)
              << Color::RESET << "\n";
    std::cout << "  当前阶段 : " << Color::BOLD
              << IndexMigration::stageToString(s.stage) << Color::RESET << "\n";
    if (!s.sourceIndex.empty()) {
        std::cout << "  旧版本   : " << s.sourceIndex << "\n";
    }
    if (!s.targetIndex.empty()) {
        std::cout << "  新版本   : " << s.targetIndex << "\n";
    }
    std::cout << "  文档数量 : 旧=" << s.progress.sourceCount
              << " 新=" << s.progress.targetCount
              << " (本进程已复制 " << s.progress.copiedInProcess
              << "，失败 " << s.progress.failedInProcess << ")\n";
    std::cout << "  读别名 " << s.readAlias << " -> ";
    if (s.readAliasPointsTo.empty()) std::cout << "(未挂载)";
    for (size_t i = 0; i < s.readAliasPointsTo.size(); ++i) {
        if (i) std::cout << ", ";
        std::cout << Color::GREEN << s.readAliasPointsTo[i] << Color::RESET;
    }
    std::cout << "\n";
    std::cout << "  写别名 " << s.writeAlias << " -> ";
    if (s.writeAliasPointsTo.empty()) std::cout << "(未挂载)";
    else std::cout << Color::GREEN << s.writeAliasPointsTo << Color::RESET;
    std::cout << "\n";
    if (!s.failureReason.empty()) {
        std::cout << "  " << Color::RED << "失败原因: " << s.failureReason
                  << Color::RESET << "\n";
    }
}

// 经别名取文档（含 _index 元数据），found=false 表示不存在
struct AliasDoc { bool found = false; std::string index; json source; };
AliasDoc getViaAlias(ESClient& client, const std::string& alias,
                     const std::string& id) {
    AliasDoc out;
    auto resp = client.rawRequest("GET", "/" + alias + "/_doc/" + id);
    if (resp.statusCode == 404) return out;
    if (!resp.isSuccess()) {
        throw ESException("getViaAlias failed: " + resp.body);
    }
    auto body = json::parse(resp.body);
    out.found = body.value("found", false);
    out.index = body.value("_index", "");
    if (out.found) out.source = body["_source"];
    return out;
}

// 迁移期间经写别名制造的新增 / 更新 / 删除
void writeTrafficDuringMigration(ESClient& client) {
    printSection("迁移期间经写别名持续写入（新增 / 更新 / 删除）");

    // 新增：文章 6
    json article6 = {
        {"title", "向量数据库选型指南"},
        {"content", "向量数据库专为高维嵌入的近似最近邻检索设计，常与大模型配合使用。"},
        {"author", "孙八"}, {"category", "技术"},
        {"tags", json::array({"向量检索", "ANN"})},
        {"created_at", "2024-06-21"}
    };
    auto r1 = client.indexDocument(WRITE_ALIAS, article6, "6");
    printInfo("经写别名新增文章 id=6，实际写入物理索引: " + r1.index);

    // 更新：文章 1 改标题
    auto r2 = client.updateDocument(WRITE_ALIAS, "1",
                                    {{"title", "人工智能的发展历程（第二版）"}});
    printInfo("经写别名更新文章 id=1，实际写入物理索引: " + r2.index);

    // 删除：文章 2
    client.deleteDocument(WRITE_ALIAS, "2");
    printInfo("经写别名删除文章 id=2");
    client.refreshIndex(WRITE_ALIAS);
}

void assertExpectedVersion(ESClient& client, const std::string& expectedIndex,
                           const std::string& label) {
    printSection("核对迁移期间写入在【" + label + "】后的版本归属（应为 " +
                 expectedIndex + "）");

    AliasDoc d6 = getViaAlias(client, READ_ALIAS, "6");
    check(d6.found && d6.index == expectedIndex,
          "新增文章 id=6 经读别名可见且位于 " + expectedIndex +
          "（实际: " + (d6.found ? d6.index : "不存在") + "）");

    AliasDoc d1 = getViaAlias(client, READ_ALIAS, "1");
    check(d1.found && d1.index == expectedIndex &&
          d1.source.value("title", "") == "人工智能的发展历程（第二版）",
          "更新文章 id=1 的新标题在 " + expectedIndex + " 生效");

    AliasDoc d2 = getViaAlias(client, READ_ALIAS, "2");
    check(!d2.found, "删除文章 id=2 在【" + label + "】后经读别名仍不可见");

    // 写别名只允许一个写索引：直接验证当前写别名指向
    std::string writeTarget = client.resolveAlias(WRITE_ALIAS);
    check(writeTarget == expectedIndex,
          "写别名唯一指向 " + expectedIndex + "（实际: " + writeTarget + "）");
}

// 通过读别名验证全文检索能力在迁移后照常工作
void demoSearchViaAlias(ESClient& client) {
    printSection("迁移后通过读别名验证全文检索");

    auto r = client.matchSearch(READ_ALIAS, "content", "人工智能");
    printInfo("Match \"人工智能\" 命中 " + std::to_string(r.total) + " 条");
    for (const auto& h : r.hits) {
        std::cout << "    • [" << h.index << "] "
                  << h.source.value("title", std::string()) << "\n";
    }
    check(r.total >= 1, "经读别名的 Match 查询正常返回");

    auto t = client.termSearch(READ_ALIAS, "category", "技术");
    check(t.total >= 1, "经读别名的 Term 查询正常返回");
}

// ==================== 各场景入口 ====================

// 初始化旧版索引（模拟线上现役 articles 物理索引，无别名）
int cmdSetup(ESClient& client) {
    for (const std::string& idx : {BASE, BASE + "-v000001", BASE + "-v000002"}) {
        if (client.indexExists(idx)) client.deleteIndex(idx);
    }
    client.createIndex(BASE, oldMapping(), indexSettings());
    auto articles = sampleArticles();
    client.bulkIndex(BASE, articles, {"1", "2", "3", "4", "5"});
    client.refreshIndex(BASE);
    printSuccess("已创建线上旧索引 '" + BASE + "' 并导入 5 篇文章（无别名）");
    return 0;
}

int cmdBegin(ESClient& client) {
    cmdSetup(client);

    IndexMigration migration(client, BASE, newMapping(), indexSettings());
    MigrationStatus s = migration.start();
    printStatus(s);
    check(!s.migrationId.empty(), "迁移已发起并返回迁移标识");
    check(s.stage == MigrationStage::COPYING, "新阶段为 COPYING（后台异步复制）");
    check(s.targetIndex == BASE + "-v000001", "新版本号为 v000001（未重复建版本）");

    // 让后台复制先跑一会儿，制造"复制进行中"的现场
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    writeTrafficDuringMigration(client);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    s = migration.getStatus();
    printStatus(s);
    printInfo("本进程在此退出，迁移状态仅保留在 Elasticsearch 中（模拟中断）");
    return 0;
}

int cmdResume(ESClient& client) {
    // 全新的进程/客户端：只凭 ES 中的状态恢复
    IndexMigration migration(client, BASE, newMapping(), indexSettings());
    MigrationStatus s = migration.start();  // 应识别 COPYING，不重建版本
    printStatus(s);
    check(!s.targetIndex.empty() && s.targetIndex == BASE + "-v000001",
          "恢复后沿用既有版本 v000001，未重复创建版本");
    check(s.stage == MigrationStage::COPYING ||
          s.stage == MigrationStage::FINAL_SYNC,
          "从 ES 识别出迁移处于复制阶段并继续");

    migration.waitForCopySettled(500, 30000);
    s = migration.verifyAndSwitch();
    printStatus(s);
    check(s.stage == MigrationStage::SWITCHED, "校验通过且原子切换完成");
    check(s.writeAliasPointsTo == BASE + "-v000001",
          "切换后写别名指向新版本");
    assertExpectedVersion(client, BASE + "-v000001", "跨进程恢复并切换");

    // 新字段在新版本上可用
    json newStyleDoc = {
        {"title", "新字段类型验证"},
        {"content", "summary 与 word_count 是本次新增的字段"},
        {"summary", "验证新增字段可写入可检索"},
        {"word_count", 42},
        {"author", "测试"}, {"category", "技术"},
        {"tags", json::array({"mapping"})},
        {"created_at", "2024-07-01"}
    };
    client.indexDocument(WRITE_ALIAS, newStyleDoc, "7");
    client.refreshIndex(WRITE_ALIAS);
    auto r = client.termSearch(READ_ALIAS, "word_count", "42");
    bool newFieldSearchable = false;
    for (const auto& h : r.hits) {
        if (h.id == "7") newFieldSearchable = true;
    }
    check(newFieldSearchable, "新字段 word_count(integer) 在新版本可检索");
    return g_failures == 0 ? 0 : 1;
}

int cmdRollback(ESClient& client) {
    IndexMigration migration(client, BASE, newMapping(), indexSettings());
    MigrationStatus s = migration.rollback();
    printStatus(s);
    check(s.stage == MigrationStage::ROLLED_BACK, "已回退到旧版本");
    check(s.writeAliasPointsTo == BASE, "回退后写别名指向旧索引 articles");
    assertExpectedVersion(client, BASE, "回退");

    // 回退期间新增写入必须落在旧版本（仍只有一个写索引）
    json article8 = {
        {"title", "回退期间的写入"},
        {"content", "该写入经写别名只能到达旧版本"},
        {"author", "周九"}, {"category", "技术"},
        {"tags", json::array({"rollback"})},
        {"created_at", "2024-07-02"}
    };
    auto w = client.indexDocument(WRITE_ALIAS, article8, "8");
    check(w.index == BASE, "回退期间新写入落在旧版本（实际: " + w.index + "）");

    // 再次原子切换（问题确认后重新前进）
    s = migration.verifyAndSwitch();
    printStatus(s);
    check(s.stage == MigrationStage::SWITCHED, "重新校验并再次切换成功");
    check(s.writeAliasPointsTo == BASE + "-v000001", "再次切换后写别名唯一指向新版本");

    bool cleaned = migration.cleanupOldVersion();
    check(cleaned, "清理旧版本索引完成（此后回退窗口关闭）");
    check(!client.indexExists(BASE), "旧物理索引 articles 已删除");
    check(client.indexExists(BASE + "-v000001"), "新版本 v000001 保留对外服务");
    return g_failures == 0 ? 0 : 1;
}

// 高并发写入不丢失：迁移期间多线程经写别名持续增/改/删，切换后核对
int cmdConcurrent(ESClient& client) {
    cmdSetup(client);
    IndexMigration migration(client, BASE, newMapping(), indexSettings());
    migration.start();

    constexpr int kWriterThreads = 4;
    constexpr int kDocsPerThread = 25;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;

    // 记录每个 id 的最终期望：>0 最终标题版本号，-1 表示应被删除
    std::map<std::string, int> expected;
    std::mutex expectedMutex;

    auto nowMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const long deadline = nowMs() + 4000;  // 持续写 4 秒，覆盖多轮复制

    for (int t = 0; t < kWriterThreads; ++t) {
        writers.emplace_back([&, t] {
            auto cli = makeClient();
            int seq = 0;
            while (!stop && nowMs() < deadline) {
                std::string id = "c" + std::to_string(t) + "-" +
                                 std::to_string(seq % kDocsPerThread);
                int ver = seq;
                json doc = {
                    {"title", "并发文档-" + id + "-v" + std::to_string(ver)},
                    {"content", "migration concurrency"},
                    {"author", "writer" + std::to_string(t)},
                    {"category", "技术"},
                    {"tags", json::array({"concurrent"})},
                    {"created_at", "2024-09-01"}
                };
                cli->indexDocument(WRITE_ALIAS, doc, id);

                bool shouldDelete = (seq % 7 == 0);  // 周期性删除
                if (shouldDelete) {
                    cli->deleteDocument(WRITE_ALIAS, id);
                }
                {
                    std::lock_guard<std::mutex> lk(expectedMutex);
                    expected[id] = shouldDelete ? -1 : ver;
                }
                ++seq;
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
        });
    }

    // 让写入覆盖复制期，然后停止写线程并切换
    std::this_thread::sleep_for(std::chrono::milliseconds(4200));
    stop = true;
    for (auto& w : writers) w.join();
    client.refreshIndex(WRITE_ALIAS);

    MigrationStatus s = migration.waitForCopySettled(200, 30000);
    s = migration.verifyAndSwitch();
    printStatus(s);
    check(s.stage == MigrationStage::SWITCHED, "高并发下迁移切换成功");

    // 逐条核对最终状态
    int presentOk = 0, deleteOk = 0, mismatches = 0;
    for (const auto& [id, want] : expected) {
        AliasDoc d = getViaAlias(client, READ_ALIAS, id);
        if (want == -1) {
            if (!d.found) ++deleteOk;
            else { ++mismatches; printError("应删除却存在: " + id); }
        } else {
            std::string wantTitle =
                "并发文档-" + id + "-v" + std::to_string(want);
            if (d.found && d.index == BASE + "-v000001" &&
                d.source.value("title", "") == wantTitle) {
                ++presentOk;
            } else {
                ++mismatches;
                printError("最终状态不符: " + id + " 期望标题 " + wantTitle);
            }
        }
    }
    printInfo("核对: 最新版本正确 " + std::to_string(presentOk) +
              " 条，删除正确 " + std::to_string(deleteOk) + " 条");
    check(mismatches == 0,
          "迁移期间并发写入的文档在切换后无丢失、无陈旧（不符 " +
          std::to_string(mismatches) + " 条）");
    return g_failures == 0 ? 0 : 1;
}

// 连续迁移：在已服务的 v1 之上再发一轮迁移，应得到 v2
int cmdChain(ESClient& client) {
    json v2Mapping = newMapping();
    v2Mapping["properties"]["views"] = {{"type", "long"}};

    IndexMigration migration(client, BASE, v2Mapping, indexSettings());
    MigrationStatus s = migration.start();  // 识别 v1 为基线，不重置数据
    printStatus(s);
    check(s.sourceIndex == BASE + "-v000001", "第二轮基线为现役 v000001");
    check(s.targetIndex == BASE + "-v000002", "新版本号递增为 v000002");

    migration.waitForCopySettled(500, 30000);
    s = migration.verifyAndSwitch();
    printStatus(s);
    check(s.stage == MigrationStage::SWITCHED, "第二轮迁移切换成功");
    check(s.writeAliasPointsTo == BASE + "-v000002", "写别名指向 v000002");
    check(s.progress.targetCount >= 5, "历史文章数据延续到 v000002");

    // 新增的 views 字段可用
    json doc = {
        {"title", "第二轮新字段"}, {"content", "views 字段"},
        {"views", 100}, {"author", "a"}, {"category", "技术"},
        {"tags", json::array()}, {"created_at", "2024-09-01"}
    };
    client.indexDocument(WRITE_ALIAS, doc, "10");
    client.refreshIndex(WRITE_ALIAS);
    auto r = client.termSearch(READ_ALIAS, "views", "100");
    bool ok = false;
    for (const auto& h : r.hits) if (h.id == "10") ok = true;
    check(ok, "第二轮新字段 views(long) 可检索");
    return g_failures == 0 ? 0 : 1;
}

int cmdStatus(ESClient& client) {
    IndexMigration migration(client, BASE, newMapping(), indexSettings());
    printStatus(migration.getStatus());
    return 0;
}

// 校验失败场景：新版本把 author 改成数值类型，旧文档全部复制失败，
// 必须拒绝切换、解除冻结，旧索引继续对外服务
json incompatibleMapping() {
    json m = oldMapping();
    m["properties"]["author"] = {{"type", "long"}};  // 旧数据是中文字符串
    return m;
}

int cmdFail(ESClient& client) {
    cmdSetup(client);

    printSection("发起一个目标 mapping 与存量数据不兼容的迁移");
    IndexMigration migration(client, BASE, incompatibleMapping(),
                             indexSettings());
    migration.start();

    // 让后台复制跑出失败项
    std::this_thread::sleep_for(std::chrono::seconds(2));

    printSection("尝试校验并切换（预期失败，旧索引继续服务）");
    MigrationStatus s = migration.verifyAndSwitch();
    printStatus(s);

    check(s.stage == MigrationStage::FAILED, "迁移阶段标记为 FAILED");
    check(!s.failureReason.empty(), "给出了失败原因: " + s.failureReason);
    check(s.writeAliasPointsTo == BASE &&
          s.readAliasPointsTo.size() == 1 &&
          s.readAliasPointsTo[0] == BASE,
          "读、写别名仍指向旧索引，旧服务未受影响");

    // 冻结已解除：经写别名仍可正常写入旧索引
    json extra = {
        {"title", "校验失败后的救命写入"},
        {"content", "切换被拒绝，写流量仍应到达旧索引"},
        {"author", "运维"}, {"category", "技术"},
        {"tags", json::array({"failover"})},
        {"created_at", "2024-08-01"}
    };
    auto w = client.indexDocument(WRITE_ALIAS, extra, "9");
    client.refreshIndex(WRITE_ALIAS);
    check(w.result == "created" && w.index == BASE,
          "失败后经写别名写入仍落在旧索引（实际: " + w.index + "）");

    AliasDoc d = getViaAlias(client, READ_ALIAS, "9");
    check(d.found && d.index == BASE, "新写入经读别名立即可见");
    check(client.indexExists(BASE + "-v000001"),
          "半成品新版本保留（未把流量切向半成品），等待人工处置");
    return g_failures == 0 ? 0 : 1;
}

// 崩溃注入场景：原子别名切换成功后、元数据落盘前进程被杀死
int cmdCrash(ESClient& client) {
    cmdSetup(client);
    IndexMigration migration(client, BASE, newMapping(), indexSettings());
    migration.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    writeTrafficDuringMigration(client);
    migration.waitForCopySettled(500, 30000);

    printInfo("开启崩溃注入：切换成功后立即 abort()");
    ::setenv("ES_MIGRATION_CRASH_AFTER_SWITCH", "1", 1);
    migration.verifyAndSwitch();  // 不会返回
    printError("不应到达此处");
    return 1;
}

// 崩溃后的新进程：从 ES 识别"别名已切、元数据停在 VERIFIED"并幂等补齐
int cmdCrashResume(ESClient& client) {
    IndexMigration migration(client, BASE, newMapping(), indexSettings());
    migration.start();  // 识别 VERIFIED 中间态，不重建版本
    MigrationStatus s = migration.verifyAndSwitch();
    printStatus(s);
    check(s.stage == MigrationStage::SWITCHED,
          "识别切换后崩溃窗口，幂等补齐为 SWITCHED（未重复冻结/对账/切别名）");
    check(s.targetIndex == BASE + "-v000001", "仍沿用版本 v000001");
    assertExpectedVersion(client, BASE + "-v000001", "崩溃恢复");
    return g_failures == 0 ? 0 : 1;
}

int cmdReset(ESClient& client) {
    // 枚举具体索引名后逐个删除（部分环境禁用通配符删除）
    auto resp = client.rawRequest(
        "GET", "/_cat/indices/" + BASE + "*?h=index&format=json");
    if (resp.isSuccess()) {
        auto arr = json::parse(resp.body);
        for (const auto& row : arr) {
            const std::string name = row.value("index", "");
            if (!name.empty()) {
                client.deleteIndex(name);
                printInfo("已删除索引: " + name);
            }
        }
    }
    printSuccess("演示索引清理完成");
    return 0;
}

// ==================== 默认：进程内端到端演示 ====================

int runDemo(ESClient& client) {
    printHeader("Elasticsearch 可恢复索引迁移 DEMO");

    // 1. 准备线上旧索引
    printSection("1. 准备线上旧索引（物理名写死为 articles，5 篇文章）");
    cmdSetup(client);

    // 2. 发起迁移
    printSection("2. 发起迁移：别名化 + 创建新版本索引 + 异步复制");
    {
        IndexMigration migration(client, BASE, newMapping(), indexSettings());
        MigrationStatus s = migration.start();
        printStatus(s);
        check(s.stage == MigrationStage::COPYING, "后台复制已启动（COPYING）");

        // 3. 复制进行中持续写入
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        writeTrafficDuringMigration(client);

        s = migration.waitForCopySettled(500, 30000);
        printInfo("复制趋于稳定，旧/新文档数量: " +
                  std::to_string(s.progress.sourceCount) + "/" +
                  std::to_string(s.progress.targetCount));
    } // 迁移对象在此析构、复制线程停止——相当于进程中断，状态只留在 ES 中

    // 4. 模拟进程中断后恢复：全新客户端视角，凭 ES 状态继续
    printSection("3. 模拟进程中断后恢复（重建迁移对象，状态来自 Elasticsearch）");
    {
        IndexMigration resumed(client, BASE, newMapping(), indexSettings());
        MigrationStatus rs = resumed.start();
        printStatus(rs);
        check(rs.targetIndex == BASE + "-v000001",
              "恢复后复用 v000001，没有产生第二个版本");

        // 5. 校验并原子切换
        printSection("4. 校验（mapping / 文档数量 / 复制失败项）并原子切换");
        resumed.waitForCopySettled(500, 30000);
        MigrationStatus s = resumed.verifyAndSwitch();
        printStatus(s);
        check(s.stage == MigrationStage::SWITCHED, "校验通过、原子切换完成");
        check(s.readAliasPointsTo.size() == 1 &&
              s.readAliasPointsTo[0] == BASE + "-v000001" &&
              s.writeAliasPointsTo == BASE + "-v000001",
              "读、写流量共同指向新版本");
        assertExpectedVersion(client, BASE + "-v000001", "切换");
        demoSearchViaAlias(client);

        // 6. 回退
        printSection("5. 切换后回退（清理旧版本之前的安全窗口）");
        s = resumed.rollback();
        printStatus(s);
        assertExpectedVersion(client, BASE, "回退");

        // 7. 再次前进并清理
        printSection("6. 重新切换并在确认后清理旧版本");
        s = resumed.verifyAndSwitch();
        check(s.stage == MigrationStage::SWITCHED, "重新切换成功");
        resumed.cleanupOldVersion();
        printSuccess("旧版本已清理，迁移生命周期结束");
        printStatus(resumed.getStatus());
    }

    printHeader(g_failures == 0 ? "演示完成，全部校验通过！"
                               : "演示完成，但存在失败断言");
    return g_failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    try {
        auto client = makeClient();
        waitForEs(*client);

        std::string cmd = argc > 1 ? argv[1] : "demo";
        if (cmd == "demo")         return runDemo(*client);
        if (cmd == "setup")        return cmdSetup(*client);
        if (cmd == "begin")        return cmdBegin(*client);
        if (cmd == "resume")       return cmdResume(*client);
        if (cmd == "rollback")     return cmdRollback(*client);
        if (cmd == "fail")         return cmdFail(*client);
        if (cmd == "crash")        return cmdCrash(*client);
        if (cmd == "crash-resume") return cmdCrashResume(*client);
        if (cmd == "chain")       return cmdChain(*client);
        if (cmd == "concurrent")  return cmdConcurrent(*client);
        if (cmd == "status")       return cmdStatus(*client);
        if (cmd == "reset")        return cmdReset(*client);

        std::cerr << "用法: es_demo [demo|setup|begin|resume|rollback|fail|"
                     "crash|crash-resume|status|reset]\n";
        return 2;
    } catch (const std::exception& e) {
        printError(std::string("程序异常: ") + e.what());
        return 1;
    }
}
