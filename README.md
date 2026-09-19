# Elasticsearch 全文检索 C++ 示例

基于 C++17 实现的 Elasticsearch 全文检索演示项目，展示如何使用 C++ 与 Elasticsearch 进行交互，实现索引管理、文档 CRUD、全文检索，以及**可恢复的在线索引迁移**（读写别名、异步复制、双写、校验切换、回退）。

## 运行方式

### 方式一：Docker Compose（推荐）

```bash
# 1. 启动所有服务（C++ 程序默认运行迁移集成场景）
docker-compose up --build -d

# 2. 查看 C++ 演示程序输出（迁移集成场景）
docker logs es-demo-cpp

# 3. 运行其他命令（全文检索演示 / 单步迁移 / 状态查看 ...）
docker-compose run --rm cpp-demo ./es_demo demo
docker-compose run --rm cpp-demo ./es_demo migrate
docker-compose run --rm cpp-demo ./es_demo status

# 4. 停止服务
docker-compose down

# 5. （可选）启用 Kibana 可视化界面
docker-compose --profile kibana up -d
```

### 方式二：本地编译运行

需要先安装依赖：libcurl-dev

```bash
# Ubuntu/Debian
sudo apt-get install libcurl4-openssl-dev

# macOS
brew install curl

# 1. 启动 Elasticsearch
docker-compose up -d elasticsearch

# 2. 编译 C++ 项目（CMake 会自动下载 nlohmann/json）
cd backend
mkdir build && cd build
cmake ..
make

# 3. 运行程序（默认打印用法）
./es_demo scenario   # 迁移集成场景
./es_demo demo       # 全文检索演示
./es_demo            # 查看全部命令
```

## 命令一览

| 命令        | 说明                                                         |
| ----------- | ------------------------------------------------------------ |
| `demo`      | 全文检索演示（读写均通过别名）                               |
| `bootstrap` | 初始化索引版本 `articles_v1` 与读写别名                      |
| `migrate`   | 发起或恢复索引迁移（断点续跑，校验通过后原子切换）           |
| `status`    | 查看迁移标识、各阶段进度、校验结论与当前别名指向             |
| `rollback`  | 切换后、清理前回退到旧版本（原子操作）                       |
| `cleanup`   | 清理旧版本（或中止/回退后的半成品）                          |
| `reset`     | 删除全部演示索引与迁移状态                                   |
| `scenario`  | 集成场景：迁移期间写入 → 切换 → 回退 → 恢复 → 校验失败       |

环境变量：

| 变量                      | 说明                                                                       |
| ------------------------- | -------------------------------------------------------------------------- |
| `ES_HOST` / `ES_PORT`     | Elasticsearch 地址（默认 `localhost:9200`）                                |
| `MIGRATE_CRASH_AFTER`     | 在指定阶段持久化后模拟进程中断（退出码 2），用于验证断点续跑               |
| `MIGRATE_DEMO_FAIL_VERIFY`| 演示用：强制校验失败，观察中止路径                                         |

`MIGRATE_CRASH_AFTER` 可选阶段：`prepare` / `copying` / `reindex`（复制任务启动后）/ `ready_to_switch` / `switched`。

## 索引迁移：可恢复的在线迁移流程

Elasticsearch 不允许直接修改既有索引的 mapping。本演示把客户端扩展为一套**可恢复的索引迁移流程**，在不停止读写的前提下完成 mapping 演进：

### 核心机制

1. **读写入口改用别名**：应用只面对 `articles_read` / `articles_write` 两个别名，物理索引带版本号（`articles_v1`、`articles_v2`…）。
2. **版本化新索引 + 异步复制**：发起迁移时创建 `articles_v2`（含新字段类型），通过 `_reindex?wait_for_completion=false` 异步复制旧数据，客户端轮询任务进度。
3. **双写镜像**：复制期间，客户端把经写别名到达的**新增、更新、删除**同步镜像到新索引（更新采用"回读整篇再写入"避免半文档）；切换后、清理前反向镜像到旧索引，保证回退不丢数据。
4. **删除对齐**：复制基于快照，复制期间被删除的文档可能被重新带入新索引；校验前对比两侧文档 ID，清理残留（"僵尸文档"）。
5. **三项校验**：目标索引**映射**与预期一致、源/目标**文档数量**一致、**复制失败项**为零。
6. **原子切换**：校验全部通过后，一次 `POST /_aliases` 调用让读写别名**共同**转向新版本；回退同样原子，写别名始终只指向一个索引。
7. **断点续跑**：迁移状态（迁移标识、阶段、复制任务 id、校验结论、历史）持久化在 ES 的 `es_demo_migration_state` 索引中。进程中断后再次执行 `migrate`，会从 ES 识别当前阶段继续——不重复创建版本、不把流量切向半成品；复制任务丢失（如 ES 重启）时安全重发（create-only 幂等）。
8. **校验失败保护**：校验不通过则进入 `aborted`，旧索引继续服务、流量不切换，并记录失败原因；可 `cleanup` 清理半成品后重试。

### 状态机

```
PREPARE → COPYING → VERIFYING → READY_TO_SWITCH → SWITCHED → CLEANED_UP
                      │                                 │
                      └─ 校验失败 → ABORTED             └─ 回退 → ROLLED_BACK → 重新校验
```

### 运维示例输出（`status`）

```
── 迁移状态 ──────────────────────────────
  迁移标识: mig_articles_v1_to_articles_v2_20260919T101530Z
  当前阶段: switched（已切换到新版本（可回退/可清理））
  源索引:   articles_v1
  目标索引: articles_v2
  复制统计: 新增 1505，冲突跳过 2，失败 0（任务 node-1:42）
  校验结论: 通过（映射✓，文档数 1505=1505✓，复制失败项 0✓）
  阶段进度:
    [2026-09-19T10:15:30Z] prepare  登记迁移 ...
    [2026-09-19T10:15:31Z] copying  异步复制启动 task=node-1:42
    [2026-09-19T10:15:33Z] ready_to_switch  校验通过（映射/文档数量/复制失败项）
    [2026-09-19T10:15:33Z] switched  读写流量已切换到 articles_v2
── 别名指向 ──────────────────────────────
  articles_read -> articles_v2
  articles_write -> articles_v2
```

### 断点续跑演示

```bash
# 在复制任务启动后模拟进程中断（退出码 2）
MIGRATE_CRASH_AFTER=reindex ./es_demo migrate
# 再次执行：从 ES 识别阶段继续，不重复创建版本
./es_demo migrate
```

### 集成场景（`scenario`）

一条命令跑通完整验证：初始化 v1 → 发起迁移 → **模拟进程中断并从 ES 恢复** → 复制期间经写别名**新增/更新/删除** → 校验 → **原子切换** → 断言迁移期间写入在新版本可查 → **原子回退** → 断言回退后数据仍在 → 再次恢复并切换 → 清理旧版本 → **校验失败场景**（保留旧索引服务并给出原因）。全程 38 项断言。

## 服务说明

| 服务          | 端口 | 说明                       |
| ------------- | ---- | -------------------------- |
| Elasticsearch | 9200 | 搜索引擎服务               |
| Kibana        | 5601 | ES 可视化管理（可选）      |
| cpp-demo      | -    | C++ 演示程序（一次性运行） |

### 访问地址

- Elasticsearch: http://localhost:9200
- Kibana: http://localhost:5601 （需使用 `--profile kibana` 启动）

## 认证说明

本项目为开发演示环境，已禁用安全认证（`xpack.security.enabled=false`），无需用户名密码即可访问。

> ⚠️ 生产环境请务必启用安全认证。

## 功能特性

### 索引管理

- ✅ 创建索引（支持自定义 mapping）
- ✅ 删除索引
- ✅ 查看索引信息
- ✅ **读写别名**（`articles_read` / `articles_write`）
- ✅ **版本化索引 + 原子别名切换**

### 在线索引迁移

- ✅ 迁移状态持久化在 ES，进程中断后断点续跑
- ✅ 异步 reindex 复制旧数据（任务丢失安全重发）
- ✅ 复制期间双写镜像（新增/更新/删除不丢失）
- ✅ 删除对齐（清理复制快照带入的残留文档）
- ✅ 三项校验（映射 / 文档数量 / 复制失败项）
- ✅ 原子切换与原子回退（写别名始终唯一）
- ✅ 校验失败保留旧索引服务并给出原因

### 文档操作

- ✅ 添加文档
- ✅ 批量添加文档
- ✅ 获取文档
- ✅ 更新文档
- ✅ 删除文档

### 全文检索

- ✅ Match 查询（分词匹配）
- ✅ Multi-Match 查询（多字段搜索）
- ✅ Term 查询（精确匹配）
- ✅ Bool 组合查询
- ✅ 高亮显示
- ✅ 分页查询

### 分词说明

本 Demo 使用 Elasticsearch 内置的 `standard` 分词器。`standard` 分词器对中文采用单字切分（Unigram），例如"人工智能"会被切分为"人"、"工"、"智"、"能"四个 token。

如需真正的中文词语切分（如将"人工智能"作为一个完整词语），需要：

1. 安装 [IK 分词器插件](https://github.com/medcl/elasticsearch-analysis-ik)
2. 修改索引 mapping 中的 `analyzer` 为 `ik_max_word`（最细粒度）或 `ik_smart`（智能切分）

示例配置见下方"扩展开发"章节。

## 技术栈

- **语言**: C++17
- **HTTP 客户端**: libcurl
- **JSON 处理**: nlohmann/json（CMake 自动下载）
- **搜索引擎**: Elasticsearch 8.11.0
- **构建工具**: CMake 3.16+
- **容器化**: Docker & Docker Compose

## 项目结构

```
.
├── backend/                    # C++ 后端代码
│   ├── CMakeLists.txt          # CMake 构建配置
│   ├── Dockerfile              # Docker 镜像构建
│   ├── include/                # 头文件
│   │   ├── es_client.hpp       # ES 客户端类（含别名/复制/双写镜像）
│   │   ├── migration_manager.hpp # 可恢复索引迁移管理器
│   │   ├── http_client.hpp     # HTTP 客户端类
│   │   └── json.hpp            # nlohmann/json 库（构建时下载）
│   ├── src/                    # 源代码
│   │   ├── main.cpp            # 主程序入口（CLI 命令分发）
│   │   ├── es_client.cpp       # ES 客户端实现
│   │   ├── migration_manager.cpp # 迁移状态机实现
│   │   └── http_client.cpp     # HTTP 客户端实现
│   └── data/                   # 示例数据
│       └── sample_data.json
├── docs/                       # 文档
│   └── project_design.md       # 项目设计文档
├── docker-compose.yml          # Docker Compose 配置
├── .gitignore                  # Git 忽略文件
└── README.md                   # 项目说明
```

## 使用示例

### 全文检索演示（`demo`）

程序运行后会自动执行以下演示：

1. **初始化** - 创建 `articles_v1` 索引与 `articles_read`/`articles_write` 别名（幂等）
2. **批量导入** - 经写别名导入示例文章数据
3. **全文检索** - 经读别名演示各种搜索方式
4. **高亮显示** - 展示搜索结果高亮
5. **文档 CRUD** - 经写别名演示增删改查

数据保留用于迁移演示，清理请运行 `./es_demo reset`。

### 输出示例

```
========================================
  Elasticsearch C++ 全文检索 DEMO
========================================

[1] 获取集群信息
  集群名称: es-demo-cluster
  ES 版本: 8.11.0
  集群状态: green
✓ 集群连接正常

[2] 初始化索引与读写别名（幂等）
✓ 创建索引 articles_v1
✓ 读写别名就绪: articles_read / articles_write → articles_v1

[3] 批量导入文档（经写别名）
✓ 导入 5 篇示例文章

[4] Match 查询演示
→ 搜索关键词: "人工智能"（经读别名 articles_read）
  命中 2 条结果 (耗时 8ms)
  [1] 人工智能的发展历程 (score: 8.23)
  [2] 深度学习入门指南 (score: 5.12)
...
```

## 扩展开发

### 启用中文分词（IK 分词器）

如需真正的中文分词能力，可以使用带 IK 分词器的 Elasticsearch 镜像：

```yaml
# docker-compose.yml 中替换 elasticsearch 镜像
elasticsearch:
  image: elasticsearch-ik:8.11.0 # 需自行构建或使用社区镜像
```

然后修改索引 mapping：

```cpp
json mapping = {
    {"properties", {
        {"title", {{"type", "text"}, {"analyzer", "ik_max_word"}}},
        {"content", {{"type", "text"}, {"analyzer", "ik_smart"}}},
        {"tags", {{"type", "keyword"}}},
        {"created_at", {{"type", "date"}}}
    }}
};
client.createIndex("my_index", mapping);
```

> 修改 mapping 后，请通过 `migrate` 命令走在线迁移流程切换到新版本，而不是直接改动旧索引。

### 添加新的搜索功能

```cpp
// 在 es_client.hpp 中添加新方法
SearchResult fuzzySearch(const std::string& index,
                         const std::string& field,
                         const std::string& value,
                         int fuzziness = 2);
```

## 许可证

MIT License
