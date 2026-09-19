#include "es_client.hpp"
#include "migration_manager.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>

using namespace es;
using json = nlohmann::json;

// ==================== 控制台颜色 ====================

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

void printSection(int num, const std::string& title) {
    std::cout << "\n" << Color::YELLOW << Color::BOLD;
    std::cout << "[" << num << "] " << title << "\n";
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

void migrationLogger(const std::string& msg) {
    std::cout << Color::MAGENTA << "  │ " << msg << Color::RESET << "\n";
}

// ==================== 常量与映射 ====================

const std::string BASE_NAME   = "articles";
const std::string READ_ALIAS  = "articles_read";
const std::string WRITE_ALIAS = "articles_write";

// v1 映射：当前线上文章结构
json v1Mappings() {
    return {
        {"properties", {
            {"title", {
                {"type", "text"},
                {"analyzer", "standard"},
                {"fields", {
                    {"keyword", {{"type", "keyword"}}}
                }}
            }},
            {"content", {
                {"type", "text"},
                {"analyzer", "standard"}
            }},
            {"author", {
                {"type", "keyword"}
            }},
            {"category", {
                {"type", "keyword"}
            }},
            {"tags", {
                {"type", "keyword"}
            }},
            {"created_at", {
                {"type", "date"},
                {"format", "yyyy-MM-dd"}
            }}
        }}
    };
}

// v2 映射：补充新的字段类型（摘要 text / 阅读量 integer / 精选 boolean）
json v2Mappings() {
    auto m = v1Mappings();
    m["properties"]["summary"]  = {{"type", "text"}, {"analyzer", "standard"}};
    m["properties"]["views"]    = {{"type", "integer"}};
    m["properties"]["featured"] = {{"type", "boolean"}};
    return m;
}

// v3 映射：演示"校验失败"场景用（再追加副标题字段）
json v3Mappings() {
    auto m = v2Mappings();
    m["properties"]["subtitle"] = {{"type", "text"}, {"analyzer", "standard"}};
    return m;
}

json indexSettings() {
    return {{"number_of_shards", 1}, {"number_of_replicas", 0}};
}

// ==================== 示例数据 ====================

std::vector<json> getSampleArticles() {
    return {
        {
            {"title", "人工智能的发展历程"},
            {"content", "人工智能（AI）是计算机科学的一个分支，致力于创建能够执行通常需要人类智能的任务的系统。从1956年达特茅斯会议开始，AI经历了多次发展浪潮。"},
            {"author", "张三"},
            {"category", "技术"},
            {"tags", json::array({"AI", "人工智能", "机器学习"})},
            {"created_at", "2024-01-15"}
        },
        {
            {"title", "深度学习入门指南"},
            {"content", "深度学习是机器学习的一个子领域，使用多层神经网络来学习数据的层次化表示。本文将介绍深度学习的基本概念和常用框架。"},
            {"author", "李四"},
            {"category", "技术"},
            {"tags", json::array({"深度学习", "神经网络", "TensorFlow"})},
            {"created_at", "2024-02-20"}
        },
        {
            {"title", "Elasticsearch 搜索引擎实战"},
            {"content", "Elasticsearch是一个分布式、RESTful风格的搜索和数据分析引擎。它能够快速地存储、搜索和分析大量数据，广泛应用于日志分析、全文搜索等场景。"},
            {"author", "王五"},
            {"category", "技术"},
            {"tags", json::array({"Elasticsearch", "搜索引擎", "全文检索"})},
            {"created_at", "2024-03-10"}
        },
        {
            {"title", "C++17 新特性详解"},
            {"content", "C++17引入了许多新特性，包括结构化绑定、if constexpr、折叠表达式等。这些特性使得C++代码更加简洁和高效。"},
            {"author", "赵六"},
            {"category", "编程语言"},
            {"tags", json::array({"C++", "C++17", "编程"})},
            {"created_at", "2024-04-05"}
        },
        {
            {"title", "微服务架构设计模式"},
            {"content", "微服务架构是一种将应用程序构建为一组小型服务的方法。每个服务运行在自己的进程中，通过轻量级机制（通常是HTTP API）进行通信。"},
            {"author", "钱七"},
            {"category", "架构"},
            {"tags", json::array({"微服务", "架构", "分布式"})},
            {"created_at", "2024-05-18"}
        }
    };
}

// 归档文章：模拟线上存量数据，让异步复制过程可观察
std::vector<json> fillerArticles(int start, int count) {
    std::vector<json> docs;
    for (int i = start; i < start + count; ++i) {
        docs.push_back({
            {"title", "归档文章 #" + std::to_string(i)},
            {"content", "这是一篇归档的历史文章，用于模拟线上存量数据。编号 " + std::to_string(i)},
            {"author", "系统"},
            {"category", "归档"},
            {"tags", json::array({"归档"})},
            {"created_at", "2023-06-15"}
        });
    }
    return docs;
}

// ==================== 公共操作 ====================

std::string aliasTarget(ESClient& client, const std::string& alias) {
    const auto indices = client.getAliasIndices(alias);
    if (indices.empty()) {
        return "(未指向任何索引)";
    }
    std::string out;
    for (size_t i = 0; i < indices.size(); ++i) {
        if (i > 0) out += ",";
        out += indices[i];
    }
    return out;
}

void printAliasStatus(ESClient& client) {
    std::cout << "  别名指向: " << READ_ALIAS << " → " << aliasTarget(client, READ_ALIAS)
              << " | " << WRITE_ALIAS << " → " << aliasTarget(client, WRITE_ALIAS) << "\n";
}

// 初始化 v1 索引与读写别名（幂等），返回是否新建
bool bootstrap(ESClient& client) {
    if (!client.getAliasIndices(WRITE_ALIAS).empty() &&
        !client.getAliasIndices(READ_ALIAS).empty()) {
        printInfo("读写别名已存在，跳过初始化");
        printAliasStatus(client);
        return false;
    }
    const std::string v1 = BASE_NAME + "_v1";
    if (!client.indexExists(v1)) {
        client.createIndex(v1, v1Mappings(), indexSettings());
        printSuccess("创建索引 " + v1);
    }
    client.updateAliases(json::array({
        {{"add", {{"index", v1}, {"alias", READ_ALIAS}}}},
        {{"add", {{"index", v1}, {"alias", WRITE_ALIAS}}}}
    }));
    printSuccess("读写别名就绪: " + READ_ALIAS + " / " + WRITE_ALIAS + " → " + v1);
    return true;
}

void importSample(ESClient& client) {
    auto articles = getSampleArticles();
    std::vector<std::string> ids = {"1", "2", "3", "4", "5"};
    auto result = client.bulkIndex(WRITE_ALIAS, articles, ids);
    client.refreshIndex(READ_ALIAS);
    printSuccess("导入 " + std::to_string(result.successCount) + " 篇示例文章");
}

void resetAll(ESClient& client) {
    if (client.indexExists("es_demo_migration_state")) {
        client.deleteIndex("es_demo_migration_state");
        printInfo("已删除迁移状态索引");
    }
    for (const auto& idx : client.listIndices(BASE_NAME + "_v*")) {
        client.deleteIndex(idx);
        printInfo("已删除索引 " + idx);
    }
}

// ==================== 检索演示（读写均通过别名） ====================

void demoClusterInfo(ESClient& client) {
    printSection(1, "获取集群信息");

    auto info = client.clusterInfo();
    std::cout << "  集群名称: " << info["cluster_name"] << "\n";
    std::cout << "  ES 版本: " << info["version"]["number"] << "\n";

    auto health = client.clusterHealth();
    std::cout << "  集群状态: " << health["status"] << "\n";
    std::cout << "  节点数量: " << health["number_of_nodes"] << "\n";

    printSuccess("集群连接正常");
}

void demoMatchSearch(ESClient& client, const std::string& indexName) {
    printSection(4, "Match 查询演示");

    std::string keyword = "人工智能";
    printInfo("搜索关键词: \"" + keyword + "\"（经读别名 " + indexName + "）");

    auto result = client.matchSearch(indexName, "content", keyword);

    std::cout << "\n  命中 " << Color::BOLD << result.total << Color::RESET
              << " 条结果 (耗时 " << result.took << "ms)\n\n";

    for (size_t i = 0; i < result.hits.size(); ++i) {
        const auto& hit = result.hits[i];
        std::cout << "  [" << (i + 1) << "] "
                  << Color::BOLD << hit.source["title"].get<std::string>() << Color::RESET
                  << " (score: " << std::fixed << std::setprecision(2) << hit.score << ")\n";
        std::cout << "      作者: " << hit.source["author"].get<std::string>()
                  << " | 分类: " << hit.source["category"].get<std::string>() << "\n";
    }
}

void demoMultiMatchSearch(ESClient& client, const std::string& indexName) {
    printSection(5, "Multi-Match 查询演示（多字段搜索）");

    std::string keyword = "深度学习";
    printInfo("搜索关键词: \"" + keyword + "\" (在 title 和 content 中)");

    auto result = client.multiMatchSearch(indexName, {"title", "content"}, keyword);

    std::cout << "\n  命中 " << Color::BOLD << result.total << Color::RESET
              << " 条结果\n\n";

    for (const auto& hit : result.hits) {
        std::cout << "  • " << Color::BOLD << hit.source["title"].get<std::string>()
                  << Color::RESET << "\n";
        std::string content = hit.source["content"].get<std::string>();
        if (content.length() > 80) {
            content = content.substr(0, 80) + "...";
        }
        std::cout << "    " << Color::CYAN << content << Color::RESET << "\n\n";
    }
}

void demoTermSearch(ESClient& client, const std::string& indexName) {
    printSection(6, "Term 查询演示（精确匹配）");

    std::string author = "王五";
    printInfo("精确匹配作者: \"" + author + "\"");

    auto result = client.termSearch(indexName, "author", author);

    std::cout << "\n  命中 " << Color::BOLD << result.total << Color::RESET
              << " 条结果\n\n";

    for (const auto& hit : result.hits) {
        std::cout << "  • " << hit.source["title"].get<std::string>() << "\n";
        std::cout << "    作者: " << hit.source["author"].get<std::string>() << "\n";
    }
}

void demoBoolSearch(ESClient& client, const std::string& indexName) {
    printSection(7, "Bool 组合查询演示");

    printInfo("查询条件: 分类='技术' AND 内容包含'学习'");

    json must = json::array({
        {{"match", {{"content", "学习"}}}}
    });
    json filter = json::array({
        {{"term", {{"category", "技术"}}}}
    });

    auto result = client.boolSearch(indexName, must, json::array(), json::array(), filter);

    std::cout << "\n  命中 " << Color::BOLD << result.total << Color::RESET
              << " 条结果\n\n";

    for (const auto& hit : result.hits) {
        std::cout << "  • " << hit.source["title"].get<std::string>() << "\n";
        std::cout << "    分类: " << hit.source["category"].get<std::string>()
                  << " | Score: " << std::fixed << std::setprecision(2) << hit.score << "\n";
    }
}

void demoHighlightSearch(ESClient& client, const std::string& indexName) {
    printSection(8, "高亮搜索演示");

    std::string keyword = "Elasticsearch";
    printInfo("搜索关键词: \"" + keyword + "\" (带高亮)");

    json query = {
        {"multi_match", {
            {"query", keyword},
            {"fields", json::array({"title", "content"})}
        }}
    };

    auto result = client.searchWithHighlight(indexName, query, {"title", "content"});

    std::cout << "\n  命中 " << Color::BOLD << result.total << Color::RESET
              << " 条结果\n\n";

    for (const auto& hit : result.hits) {
        std::cout << "  标题: " << hit.source["title"].get<std::string>() << "\n";
        if (hit.highlight.contains("content")) {
            std::cout << "  高亮: ";
            for (const auto& fragment : hit.highlight["content"]) {
                std::string text = fragment.get<std::string>();
                size_t pos = 0;
                while ((pos = text.find("<em>", pos)) != std::string::npos) {
                    text.replace(pos, 4, Color::RED + Color::BOLD);
                    pos += Color::RED.length() + Color::BOLD.length();
                }
                pos = 0;
                while ((pos = text.find("</em>", pos)) != std::string::npos) {
                    text.replace(pos, 5, Color::RESET);
                    pos += Color::RESET.length();
                }
                std::cout << text << "\n";
            }
        }
        std::cout << "\n";
    }
}

void demoDocumentCRUD(ESClient& client, const std::string& indexName) {
    printSection(9, "文档 CRUD 操作演示（经写别名）");

    printInfo("创建新文档...");
    json newDoc = {
        {"title", "测试文档"},
        {"content", "这是一个用于演示CRUD操作的测试文档"},
        {"author", "测试用户"},
        {"category", "测试"},
        {"tags", json::array({"test", "demo"})},
        {"created_at", "2024-06-01"}
    };

    auto createResult = client.indexDocument(indexName, newDoc, "test-doc-1");
    printSuccess("文档创建成功, ID: " + createResult.id);

    client.refreshIndex(indexName);

    printInfo("读取文档...");
    auto doc = client.getDocument(indexName, "test-doc-1");
    if (doc) {
        printSuccess("文档读取成功: " + (*doc)["title"].get<std::string>());
    }

    printInfo("更新文档...");
    json updateData = {{"title", "更新后的测试文档"}};
    auto updateResult = client.updateDocument(indexName, "test-doc-1", updateData);
    printSuccess("文档更新成功, 版本: " + std::to_string(updateResult.version));

    printInfo("删除文档...");
    if (client.deleteDocument(indexName, "test-doc-1")) {
        printSuccess("文档删除成功");
    }
}

// ==================== 命令：demo ====================

int cmdDemo(ESClient& client) {
    printHeader("Elasticsearch C++ 全文检索 DEMO");

    demoClusterInfo(client);

    printSection(2, "初始化索引与读写别名（幂等）");
    bootstrap(client);

    // 若迁移正在进行，写操作按 ES 中的迁移状态自动双写
    MigrationManager::attachWriteMirrorFor(client, BASE_NAME);

    printSection(3, "批量导入文档（经写别名）");
    importSample(client);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    demoMatchSearch(client, READ_ALIAS);
    demoMultiMatchSearch(client, READ_ALIAS);
    demoTermSearch(client, READ_ALIAS);
    demoBoolSearch(client, READ_ALIAS);
    demoHighlightSearch(client, READ_ALIAS);
    demoDocumentCRUD(client, WRITE_ALIAS);

    printSection(10, "演示数据保留说明");
    printInfo("数据保留用于迁移演示（migrate/status/rollback/cleanup）");
    printInfo("如需清空环境，请运行: ./es_demo reset");

    printHeader("演示完成！");
    return 0;
}

// ==================== 命令：bootstrap / reset / status / migrate / rollback / cleanup ====================

int cmdBootstrap(ESClient& client) {
    printHeader("初始化索引版本与读写别名");
    bootstrap(client);
    importSample(client);
    printAliasStatus(client);
    return 0;
}

int cmdReset(ESClient& client) {
    printHeader("重置演示环境");
    resetAll(client);
    printSuccess("环境已重置");
    return 0;
}

int cmdStatus(ESClient& client) {
    printHeader("迁移状态");
    MigrationManager manager(client, BASE_NAME);
    manager.setLogCallback(migrationLogger);
    manager.printStatus();
    return 0;
}

int cmdMigrate(ESClient& client) {
    printHeader("索引迁移（断点续跑）");
    MigrationManager manager(client, BASE_NAME);
    manager.setLogCallback(migrationLogger);
    bool ok = manager.run(v2Mappings(), indexSettings());
    manager.printStatus();
    if (ok) {
        printSuccess("迁移流程执行成功");
        return 0;
    }
    printError("迁移未通过校验，旧索引继续服务");
    return 1;
}

int cmdRollback(ESClient& client) {
    printHeader("回退到旧版本");
    MigrationManager manager(client, BASE_NAME);
    manager.setLogCallback(migrationLogger);
    bool ok = manager.rollbackStep();
    manager.printStatus();
    return ok ? 0 : 1;
}

int cmdCleanup(ESClient& client) {
    printHeader("清理旧版本 / 半成品");
    MigrationManager manager(client, BASE_NAME);
    manager.setLogCallback(migrationLogger);
    bool ok = manager.cleanupStep();
    manager.printStatus();
    return ok ? 0 : 1;
}

// ==================== 命令：scenario（集成场景） ====================

int cmdScenario(ESClient& client, const std::string& host, int port) {
    int failures = 0;
    auto check = [&](bool cond, const std::string& name) {
        if (cond) {
            printSuccess("断言: " + name);
        } else {
            printError("断言失败: " + name);
            failures++;
        }
    };

    printHeader("集成场景：可恢复的索引迁移");

    // ---------- 第 0 步：环境重置 ----------
    printSection(0, "环境重置");
    resetAll(client);
    printSuccess("历史索引与迁移状态已清理");

    // ---------- 第 1 步：初始化 v1 ----------
    printSection(1, "初始化 articles_v1 与读写别名");
    bootstrap(client);
    importSample(client);

    printInfo("补充 1500 篇归档文章（模拟线上存量）...");
    for (int batch = 0; batch < 3; ++batch) {
        auto docs = fillerArticles(batch * 500, 500);
        std::vector<std::string> ids;
        for (int i = batch * 500; i < batch * 500 + 500; ++i) {
            ids.push_back("arc-" + std::to_string(i));
        }
        client.bulkIndex(WRITE_ALIAS, docs, ids);
    }
    client.refreshIndex(READ_ALIAS);
    check(client.countDocuments(READ_ALIAS) == 1505, "初始文档数为 1505");
    printAliasStatus(client);

    // ---------- 第 2 步：发起迁移（登记 + 建目标 + 启动异步复制） ----------
    printSection(2, "发起迁移 v1 → v2（新增 summary/views/featured 字段）");
    std::string migrationId;
    {
        MigrationManager manager(client, BASE_NAME);
        manager.setLogCallback(migrationLogger);
        manager.prepareStep(v2Mappings(), indexSettings());
        migrationId = manager.migrationId();
        check(manager.phase() == MigrationPhase::COPYING, "迁移进入 COPYING（异步复制中）");
        check(client.indexExists("articles_v2"), "带版本号的目标索引 articles_v2 已创建");
    }
    printInfo("【模拟进程中断】丢弃内存中的迁移管理器，重新执行");

    // ---------- 第 3 步：进程重启 → 从 ES 恢复；复制期间经写别名写入 ----------
    printSection(3, "进程重启：从 Elasticsearch 识别阶段并继续");
    MigrationManager manager2(client, BASE_NAME);
    manager2.setLogCallback(migrationLogger);
    manager2.prepareStep(v2Mappings(), indexSettings());  // 恢复执行：不重复创建版本
    check(manager2.migrationId() == migrationId, "恢复后迁移标识不变: " + migrationId);
    check(manager2.state().value("target_index", "") == "articles_v2",
          "恢复后目标版本不变（未重复创建版本）");

    printInfo("复制期间，业务经写别名持续写入（新增/更新/删除）...");
    std::atomic<bool> writerFailed{false};
    std::string writerError;
    std::thread writer([&]() {
        try {
            // 独立线程使用独立客户端（HttpClient 非线程安全）
            ESClient writerClient(host, port);
            // 业务侧按 ES 中的迁移状态挂载双写镜像
            MigrationManager::attachWriteMirrorFor(writerClient, BASE_NAME);

            // 新增 2 篇（含新字段类型）
            writerClient.indexDocument(WRITE_ALIAS, {
                {"title", "迁移期间新增：向量检索实践"},
                {"content", "迁移期间写入的文章，介绍向量检索与 Embedding 的应用。"},
                {"author", "孙八"},
                {"category", "技术"},
                {"tags", json::array({"向量", "检索"})},
                {"created_at", "2024-07-01"},
                {"summary", "迁移期间新增文章的摘要"},
                {"views", 100},
                {"featured", true}
            }, "mig-new-1");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            writerClient.indexDocument(WRITE_ALIAS, {
                {"title", "迁移期间新增：搜索平台稳定性建设"},
                {"content", "迁移期间写入的文章，介绍搜索平台稳定性建设的实践。"},
                {"author", "周九"},
                {"category", "架构"},
                {"tags", json::array({"稳定性", "搜索"})},
                {"created_at", "2024-07-02"},
                {"summary", "稳定性建设实践摘要"},
                {"views", 42},
                {"featured", false}
            }, "mig-new-2");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // 更新 1 篇
            writerClient.updateDocument(WRITE_ALIAS, "2", {
                {"title", "深度学习入门指南（迁移期间修订）"},
                {"views", 256}
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // 删除 2 篇：一篇通常已被复制、一篇尚未被复制（验证删除对齐）
            writerClient.deleteDocument(WRITE_ALIAS, "4");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            writerClient.deleteDocument(WRITE_ALIAS, "arc-1499");
        } catch (const std::exception& e) {
            writerFailed = true;
            writerError = e.what();
        }
    });

    manager2.copyStep();   // 等待异步复制完成
    writer.join();
    if (writerFailed) {
        printError("写入线程异常: " + writerError);
        failures++;
    }
    check(manager2.verifyStep(), "校验通过（映射 / 文档数量 / 复制失败项）");
    manager2.switchStep();
    check(manager2.phase() == MigrationPhase::SWITCHED, "读写流量已原子切换到 articles_v2");
    manager2.printStatus();

    // ---------- 第 4 步：切换后验证（经读别名 → v2） ----------
    printSection(4, "切换后验证：迁移期间的写入在新版本可查");
    check(aliasTarget(client, READ_ALIAS) == "articles_v2", "读别名指向 articles_v2");
    check(aliasTarget(client, WRITE_ALIAS) == "articles_v2", "写别名指向 articles_v2（唯一写索引）");
    {
        auto doc = client.getDocument(READ_ALIAS, "mig-new-1");
        check(doc.has_value(), "迁移期间新增的文章在新版本存在");
        if (doc) {
            check((*doc)["views"] == 100 && (*doc)["featured"] == true,
                  "新字段 views/featured 已生效");
        }
        auto updated = client.getDocument(READ_ALIAS, "2");
        check(updated && (*updated)["title"] == "深度学习入门指南（迁移期间修订）",
              "迁移期间的更新在切换后保留");
        check(updated && (*updated)["views"] == 256, "更新的新字段值在切换后保留");
        check(!client.getDocument(READ_ALIAS, "4"), "迁移期间删除的文章在切换后不存在");
        check(!client.getDocument(READ_ALIAS, "arc-1499"), "删除的归档文章未残留（删除对齐生效）");
        check(client.countDocuments(READ_ALIAS) == 1505, "切换后文档总数符合预期 1505");
        check(client.matchSearch(READ_ALIAS, "content", "向量检索").total >= 1,
              "新文章可被全文检索到");
    }

    // ---------- 第 5 步：回退 ----------
    printSection(5, "回退到旧版本（清理前允许回退）");
    check(manager2.rollbackStep(), "原子回退成功");
    check(aliasTarget(client, READ_ALIAS) == "articles_v1", "回退后读别名指向 articles_v1");
    check(aliasTarget(client, WRITE_ALIAS) == "articles_v1", "回退后写别名指向 articles_v1（唯一写索引）");
    {
        auto doc = client.getDocument(READ_ALIAS, "mig-new-1");
        check(doc.has_value(), "回退后迁移期间新增的文章仍在旧版本");
        auto updated = client.getDocument(READ_ALIAS, "2");
        check(updated && (*updated)["title"] == "深度学习入门指南（迁移期间修订）",
              "回退后迁移期间的更新仍在");
        check(!client.getDocument(READ_ALIAS, "4"), "回退后删除仍生效");
        check(client.countDocuments(READ_ALIAS) == 1505, "回退后文档总数一致 1505");
    }
    manager2.printStatus();

    // ---------- 第 6 步：再次重启进程 → 恢复并重新切换、清理 ----------
    printSection(6, "进程重启：从回退状态恢复，重新切换并清理");
    {
        MigrationManager manager3(client, BASE_NAME);
        manager3.setLogCallback(migrationLogger);
        check(manager3.run(v2Mappings(), indexSettings()), "从 ROLLED_BACK 恢复迁移成功");
        check(manager3.phase() == MigrationPhase::SWITCHED, "重新切换到 articles_v2");
        check(aliasTarget(client, WRITE_ALIAS) == "articles_v2", "写别名再次指向 articles_v2");
        check(client.getDocument(READ_ALIAS, "mig-new-1").has_value(),
              "重新切换后迁移期间写入的文章仍在");

        check(manager3.cleanupStep(), "清理旧版本成功");
        check(!client.indexExists("articles_v1"), "旧索引 articles_v1 已删除");
        check(aliasTarget(client, READ_ALIAS) == "articles_v2", "清理后读别名仍指向 articles_v2");
        check(client.getDocument(READ_ALIAS, "mig-new-1").has_value(), "清理后数据完整");
        manager3.printStatus();
    }

    // ---------- 第 7 步：校验失败场景 ----------
    printSection(7, "校验失败场景：保留旧索引服务并给出原因");
    setenv("MIGRATE_DEMO_FAIL_VERIFY", "count", 1);
    bool migrateOk;
    MigrationManager manager4(client, BASE_NAME);
    manager4.setLogCallback(migrationLogger);
    migrateOk = manager4.run(v3Mappings(), indexSettings());  // v2 → v3
    unsetenv("MIGRATE_DEMO_FAIL_VERIFY");
    check(!migrateOk, "校验失败时迁移中止");
    check(manager4.phase() == MigrationPhase::ABORTED, "迁移状态为 ABORTED");
    check(aliasTarget(client, READ_ALIAS) == "articles_v2" &&
          aliasTarget(client, WRITE_ALIAS) == "articles_v2",
          "校验失败后读写流量保持旧版本（唯一写索引）");
    check(client.getDocument(READ_ALIAS, "mig-new-1").has_value(), "旧版本服务不受影响");
    check(manager4.cleanupStep(), "清理中止的迁移成功");
    check(!client.indexExists("articles_v3"), "半成品 articles_v3 已删除");
    manager4.printStatus();

    // ---------- 汇总 ----------
    if (failures == 0) {
        printHeader("集成场景全部通过 ✓");
    } else {
        printHeader("集成场景存在 " + std::to_string(failures) + " 项失败");
    }
    return failures == 0 ? 0 : 1;
}

// ==================== 用法 ====================

void printUsage() {
    std::cout << R"(
Elasticsearch C++ 全文检索与索引迁移演示

用法: es_demo <命令>

命令:
  demo       全文检索演示（读写均通过别名）
  bootstrap  初始化索引版本 articles_v1 与读写别名
  migrate    发起或恢复索引迁移（断点续跑，校验通过后原子切换）
  status     查看迁移标识、各阶段进度、校验结论与别名指向
  rollback   切换后、清理前回退到旧版本（原子操作）
  cleanup    清理旧版本（或中止/回退后的半成品）
  reset      删除全部演示索引与迁移状态
  scenario   集成场景：迁移期间写入 → 切换 → 回退 → 恢复 → 校验失败

环境变量:
  ES_HOST / ES_PORT            Elasticsearch 地址（默认 localhost:9200）
  MIGRATE_CRASH_AFTER=<阶段>   在指定阶段持久化后模拟进程中断（退出码 2），
                               阶段名: prepare/copying/reindex/ready_to_switch/switched
                               （reindex = 复制任务启动后中断，用于验证任务丢失恢复）
  MIGRATE_DEMO_FAIL_VERIFY=1   演示用：强制校验失败，观察中止路径
)";
}

// ==================== 主函数 ====================

int main(int argc, char** argv) {
    const char* esHost = std::getenv("ES_HOST");
    const char* esPort = std::getenv("ES_PORT");
    std::string host = esHost ? esHost : "localhost";
    int port = esPort ? std::stoi(esPort) : 9200;

    if (argc < 2) {
        printUsage();
        return 1;
    }
    const std::string command = argv[1];
    if (command == "help" || command == "-h" || command == "--help") {
        printUsage();
        return 0;
    }

    std::cout << "\n连接到 Elasticsearch: " << host << ":" << port << "\n";

    try {
        ESClient client(host, port);

        // 等待 ES 就绪
        std::cout << "等待 Elasticsearch 就绪";
        int retries = 30;
        while (!client.ping() && retries > 0) {
            std::cout << "." << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            retries--;
        }
        std::cout << "\n";
        if (retries == 0) {
            printError("无法连接到 Elasticsearch，请确保服务已启动");
            return 1;
        }

        if (command == "demo")      return cmdDemo(client);
        if (command == "bootstrap") return cmdBootstrap(client);
        if (command == "reset")     return cmdReset(client);
        if (command == "status")    return cmdStatus(client);
        if (command == "migrate")   return cmdMigrate(client);
        if (command == "rollback")  return cmdRollback(client);
        if (command == "cleanup")   return cmdCleanup(client);
        if (command == "scenario")  return cmdScenario(client, host, port);

        printError("未知命令: " + command);
        printUsage();
        return 1;
    } catch (const std::exception& e) {
        printError(std::string("程序异常: ") + e.what());
        return 1;
    }
}
