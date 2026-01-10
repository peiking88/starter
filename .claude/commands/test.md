---
name: /test
description: 运行Seastar项目测试和基准测试
allowed-tools: [Bash, Execute]
---

# Seastar项目测试命令

## 运行所有程序
```bash
# 确保已构建
cd build

# 测试文件分割器
dd if=/dev/zero of=input.dat bs=4096 count=50000
./big_file_splitter --input ../input.dat -m500 -c5 --memory-pct 1.0

# 测试素数计算器
./prime_calculator -t 1000

# 运行性能基准测试
./prime_bench -t 8 -n 2500000 -c4
```

## 验证结果一致性
```bash
# 检查所有实现是否产生相同结果
cd build
./prime_bench -t 4 -n 1000000 -c2
# 验证输出中的"Result consistency: PASSED"
```

## 性能分析
```bash
# 使用不同参数测试性能
./prime_bench -t 16 -n 1250000 -c8
./prime_bench -t 32 -n 625000 -c16
```

## 清理测试文件
```bash
rm -f input.dat chunk.*
```