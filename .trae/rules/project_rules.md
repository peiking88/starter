---
description: Seastar Starter Project Rules
alwaysApply: true
enabled: true
updatedAt: 2026-03-30T12:20:00.000Z
provider: trae
---

# 项目规则摘要

## 核心规则

1. **GitHub 访问**: 使用 `github.com` 域名
2. **Git 配置**: 用户名 `peiking88`，凭据使用环境变量或 git credential store
3. **语言规范**: 工作中文输出，提交/README 用英文
4. **编译参数**: `-j$(nproc)` 并行编译
5. **第三方保护**: 禁止修改 `3rdparty/` 和 `external/`
6. **测试规范**: 确保所有单元测试正确执行，不许跳过
7. **调试规范**: 不许简化或绕过问题

## 构建命令
```bash
./build.sh -r              # Release 构建
./build.sh -d              # Debug 构建
./build.sh -a              # 构建 Release 和 Debug
./build.sh -r -c           # 清理后重新构建
```

## 目录结构
```
src/           # 源文件
3rdparty/      # 第三方库 (禁止修改)
build/release/ # Release 输出
build/debug/   # Debug 输出
```

## 测试命令
```bash
./build/release/prime_bench -t 4 -n 100000 -c 4
```
