# Elasticsearch 全文检索 C++ 示例

基于 C++17 实现的 Elasticsearch 全文检索演示项目，展示如何使用 C++ 与 Elasticsearch 进行交互，实现索引管理、文档 CRUD、全文检索，以及一套**可恢复的在线索引迁移（Reindex + Alias）流程**。

## 运行方式

### 方式一：Docker Compose（推荐）

```bash
# 1. 启动所有服务
docker-compose up --build -d

# 2. 查看 C++ 演示程序输出（默认执行完整迁移演示）
docker logs es-demo-cpp

# 3. 停止服务
docker-compose down

# 4. （可选）启用 Kibana 可视化界面
docker-compose --profile kibana up -d
```

### 方式二：本地编译运行

需要先安装依赖：libcurl-dev（nlohmann/json 头文件已随仓库提供在 `backend/include/json.hpp`）

```bash
# Ubuntu/Debian
sudo apt-get install libcurl4-openssl-dev cmake

# 1. 启动 Elasticsearch
docker-compose up -d elasticsearch

# 2. 编译 C++ 项目
cd backend
mkdir build && cd build
cmake ..
make

# 3. 运行程序（默认完整端到端演示）
./es_demo
```


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
- **JSON 处理**: nlohmann/json 3.x（头文件随仓库提供）
- **搜索引擎**: Elasticsearch 8.11.0
- **构建工具**: CMake 3.16+
- **容器化**: Docker & Docker Compose

## 项目结构

```
.
├── backend/                 # C++ 后端代码
│   ├── CMakeLists.txt      # CMake 构建配置
│   ├── Dockerfile          # Docker 镜像构建
│   ├── include/            # 头文件
│   │   ├── es_client.hpp   # ES 客户端类
│   │   ├── http_client.hpp # HTTP 客户端类
│   │   └── json.hpp        # nlohmann/json 库
│   ├── src/                # 源代码
│   │   ├── main.cpp        # 主程序入口
│   │   ├── es_client.cpp   # ES 客户端实现
│   │   └── http_client.cpp # HTTP 客户端实现
│   └── data/               # 示例数据
│       └── sample_data.json
├── docs/                   # 文档
│   └── project_design.md   # 项目设计文档
├── docker-compose.yml      # Docker Compose 配置
├── .gitignore             # Git 忽略文件
└── README.md              # 项目说明
```

## 使用示例

程序默认（`es_demo demo`）演示完整的可恢复迁移生命周期：

1. 准备线上旧索引（物理名写死为 `articles`，5 篇文章，无别名）
2. 发起迁移：读写入口切到别名、创建带版本号的新索引 `articles-v000001`、后台异步复制
3. 复制进行中经**写别名**持续写入（新增 / 更新 / 删除各一）
4. 模拟进程中断，用全新对象从 Elasticsearch 识别阶段继续（不重复建版本）
5. 校验（mapping / 文档数量 / 复制失败项）通过后，**一次原子别名操作**切换读写流量
6. 核对迁移期间写入在切换后的版本归属
7. 原子**回退**到旧版本并再次核对，随后重新前进并清理旧版本

### 命令一览（分阶段/跨进程集成场景）

| 命令 | 作用 |
| ---- | ---- |
| `demo` | 单进程端到端演示（含进程中断恢复、切换、回退、再切换、清理） |
| `setup` | 仅初始化线上旧索引 `articles` |
| `begin` | 发起迁移、跑一会儿后台复制并制造增量写入后退出（状态只留在 ES） |
| `resume` | 全新进程：从 ES 识别阶段继续，完成校验与原子切换 |
| `rollback` | 全新进程：回退到旧版本、验证、再切换、清理旧版本 |
| `fail` | 目标 mapping 与存量数据不兼容：复制失败→拒绝切换、旧索引继续服务 |
| `crash` / `crash-resume` | 注入"原子切换成功后、元数据落盘前崩溃"，再由新进程幂等补齐 |
| `chain` | 在已服务的 v1 上再发一轮迁移，验证版本递增为 v2、数据延续 |
| `status` | 从 ES 重建并打印迁移标识、阶段、进度、别名指向 |
| `reset` | 删除所有演示索引 |

跨进程示例：

```bash
./es_demo begin     # 进程1：发起迁移后退出
./es_demo resume    # 进程2：识别 COPYING/FINAL_SYNC，继续并切换
./es_demo rollback  # 进程3：回退 / 再切换 / 清理
```

### 可恢复在线索引迁移设计

Elasticsearch 不允许修改既有 mapping，因此通过"**新版本索引 + 别名 + 在线复制 + 原子切换**"完成字段类型演进：

```
                迁移期间                              切换后
写流量 ──► articles-write ──► articles(旧)      articles-write ──► articles-v000001(新)
读流量 ──► articles-read  ──► articles(旧)      articles-read  ──► articles-v000001(新)
                              后台 scroll+bulk 幂等复制 + 对账删除 ──►
```

关键保证：

- **别名化读写**：业务读写只认 `articles-read` / `articles-write`，不感知物理索引名；写别名任意时刻只关联一个索引（`is_write_index=true`），杜绝两个写索引。
- **版本化索引**：物理索引名 `articles-v000001`、`-v000002` …，迁移创建新版本而非覆盖。
- **不丢增量写入**：后台线程持续做幂等对账（按 `_id` 覆盖复制新增/更新，并删除目标中源已不存在的文档）；切换前先对旧索引加 `blocks.write` 冻结，refresh 后做最后一次对账，把迁移期间经写别名到达的增删改全部追平。
- **三项校验闸门**：复制失败项为 0、旧/新文档数量相等、期望 mapping 字段与类型齐全（并逐 id 比对 `_source`），全部通过才切换。
- **原子切换**：读、写别名的 remove/add 放在**同一个 `_aliases` 调用**里，Elasticsearch 保证整体生效或整体不生效；动作按实时别名差集生成，崩溃后重试天然幂等。
- **状态外置、可恢复**：迁移标识、阶段（COPYING/FINAL_SYNC/VERIFIED/SWITCHED/ROLLED_BACK/FAILED）、旧版本名都写入新版本索引 mapping 的 `_meta`。进程重启后完全从 ES 识别当前阶段：COPYING/FINAL_SYNC 继续复制，VERIFIED+别名已切则幂等补齐为 SWITCHED，绝不重复创建版本或把流量切向半成品。
- **失败安全**：校验失败或最终同步异常时解除冻结、别名不动，旧索引继续对外服务，并在 `_meta` 记录原因；半成品新版本保留待人工处置。
- **可回退**：切换后旧版本仍保留，回退用一次原子别名操作把读写流量切回旧版本（同样只有一个写索引）；`cleanupOldVersion()` 确认无误后才删除旧版本、关闭回退窗口。

### 输出示例

```
--- 4. 校验（mapping / 文档数量 / 复制失败项）并原子切换 ---
  迁移标识 : mig-a530c635e4ec34f3dce100700aa33430
  当前阶段 : SWITCHED
  旧版本   : articles
  新版本   : articles-v000001
  文档数量 : 旧=5 新=5 (本进程已复制 5，失败 0)
  读别名 articles-read  -> articles-v000001
  写别名 articles-write -> articles-v000001
✓ 校验通过、原子切换完成
✓ 新增文章 id=6 经读别名可见且位于 articles-v000001
✓ 更新文章 id=1 的新标题在 articles-v000001 生效
✓ 删除文章 id=2 在切换后经读别名仍不可见
✓ 写别名唯一指向 articles-v000001
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
