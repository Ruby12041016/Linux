# MySQL 常用语法完整汇总
> 环境：MySQL 8.0 / 5.7，字符集 `utf8mb4`；区分 **反引号`` ` ``（表/字段）、单引号`'`（字符串）**

## 一、基础概念复习
1. `` `名称` `` 反引号：包裹**数据库名、表名、字段名**，防止和MySQL关键字冲突
2. `'文本'` 单引号：包裹**字符串、日期常量**
3. 注释：`-- 单行注释` / `/* 多行注释 */`
4. 大小写：MySQL Windows默认不区分表名字母；Linux区分，建议统一小写

---
# 1. 数据库操作
## 1.1 创建数据库
```sql
CREATE DATABASE IF NOT EXISTS chat_room 
DEFAULT CHARACTER SET utf8mb4 
DEFAULT COLLATE utf8mb4_unicode_ci;
```
- `IF NOT EXISTS`：不存在才创建，避免重复创建报错
- `utf8mb4`：支持emoji表情

## 1.2 切换数据库
```sql
USE chat_room;
```

## 1.3 查看所有数据库
```sql
SHOW DATABASES;
```

## 1.4 删除数据库（谨慎！数据全部清空）
```sql
DROP DATABASE IF EXISTS chat_room;
```

---
# 2. 数据表操作
## 2.1 创建表 CREATE TABLE
```sql
CREATE TABLE IF NOT EXISTS `t_user` (
  `uid` BIGINT NOT NULL AUTO_INCREMENT COMMENT '用户ID',
  `username` VARCHAR(50) NOT NULL COMMENT '账号',
  `nickname` VARCHAR(50) DEFAULT '' COMMENT '昵称',
  PRIMARY KEY (`uid`),
  UNIQUE KEY `uk_username`(`username`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户表';
```
关键字解释：
- `IF NOT EXISTS`：表不存在才创建
- `AUTO_INCREMENT`：自增数字，一般用于主键ID
- `NOT NULL`：此字段**不能为空**
- `DEFAULT ''`：不传值时，默认填充空字符串
- `COMMENT`：注释，方便看懂表结构
- `PRIMARY KEY`：主键，唯一标识一行数据，查询极快
- `UNIQUE KEY`：唯一约束，该字段不能重复（账号不能重复）
- `ENGINE=InnoDB`：支持事务、外键（聊天室必须用InnoDB）

## 2.2 查看表结构
```sql
DESC t_user;
-- 完整建表语句
SHOW CREATE TABLE t_user;
```

## 2.3 删除表
```sql
DROP TABLE IF EXISTS t_user;
```

## 2.4 修改表 ALTER TABLE
```sql
-- 添加字段
ALTER TABLE `t_user` ADD `avatar` VARCHAR(256) DEFAULT '' COMMENT '头像';
-- 删除字段
ALTER TABLE `t_user` DROP COLUMN `avatar`;
-- 修改字段类型/名称
ALTER TABLE `t_user` MODIFY `nickname` VARCHAR(60);
```

## 2.5 创建索引 KEY（优化查询速度）
```sql
-- 普通索引：加快where条件查询
CREATE INDEX idx_from_uid ON t_chat_msg(`from_uid`);
-- 唯一索引：值不能重复
CREATE UNIQUE INDEX uk_name ON t_user(`username`);
-- 删除索引
DROP INDEX idx_from_uid ON t_chat_msg;
```
> 不要盲目建索引：索引提升查询、**降低插入/更新速度**

---
# 3. DML 增删改查（业务核心！C++代码频繁调用）
## 3.1 插入数据 INSERT
```sql
-- 标准写法（推荐）
INSERT INTO `t_user` (`username`,`nickname`) 
VALUES ('zhangsan','张三');

-- 多条批量插入（性能更高）
INSERT INTO `t_user` (`username`,`nickname`) 
VALUES ('lisi','李四'),('wangwu','王五');

-- 存在则更新
INSERT INTO `t_user` (`uid`,`nickname`) VALUES (1,'新昵称')
ON DUPLICATE KEY UPDATE `nickname`='新昵称';
```

## 3.2 查询 SELECT（最多！）
基础模板
```sql
SELECT 字段1,字段2 FROM 表名 WHERE 条件;
-- * 代表所有字段（正式项目尽量不要用*，指定字段节省带宽）
SELECT `uid`,`username` FROM `t_user`;
```

### WHERE 条件运算符
| 符号 | 作用 |
|------|------|
| `=` | 等于 |
| `!=` `<>` | 不等于 |
| `>` `<` `>=` `<=` | 大小比较 |
| `AND` | 并且 |
| `OR` | 或者 |
| `IS NULL` | 判断为空 |
| `IS NOT NULL` | 判断非空 |
| `LIKE '%关键词%'` | 模糊搜索 |

示例
```sql
-- 查询uid=1001的用户
SELECT * FROM t_user WHERE uid=1001;

-- 模糊查询昵称带"张"
SELECT * FROM t_user WHERE nickname LIKE '%张%';

-- 私聊历史查询
SELECT * FROM t_chat_msg 
WHERE (from_uid=1001 AND to_uid=1002) OR (from_uid=1002 AND to_uid=1001)
ORDER BY msg_time DESC LIMIT 20;
```

### 排序 ORDER BY
```sql
ORDER BY msg_time DESC; -- DESC倒序（最新消息在前）
ORDER BY msg_time ASC;  -- ASC正序（旧消息在前）
```

### 分页 LIMIT（加载历史消息必备）
```sql
-- 跳过0条，取20条
SELECT * FROM t_chat_msg WHERE room_id=100 ORDER BY msg_time DESC LIMIT 0,20;
```

### 聚合函数
```sql
COUNT(*) 统计行数
MAX() 最大值
MIN() 最小值
SUM() 求和
```
示例：统计房间消息总数
```sql
SELECT COUNT(*) FROM t_chat_msg WHERE room_id=100;
```

## 3.3 更新数据 UPDATE
>  **极度重要：不加WHERE会更新整张表所有数据！**
```sql
-- 正确：只更新uid=1的昵称
UPDATE `t_user` SET `nickname`='新名字' WHERE `uid`=1;
```

## 3.4 删除数据 DELETE
>  **不加WHERE清空整张表！谨慎执行**
```sql
DELETE FROM `t_offline_msg` WHERE recv_uid=1001;
```

> 区分：
> `DELETE`：删除行，可以带条件；
> `TRUNCATE t_user`：清空整张表，无法回滚，速度更快。

---
# 4. 事务
**作用：多条SQL要么全部成功，要么全部失败，防止数据错乱**
场景举例：发送消息，同时写入聊天表+离线消息表，希望原子执行。
```sql
START TRANSACTION; -- 开启事务

INSERT INTO t_chat_msg(...) VALUES(...);
INSERT INTO t_offline_msg(...) VALUES(...);

COMMIT; -- 全部执行成功，提交

-- 中途出错执行：ROLLBACK; 回滚，全部撤销
```

---
# 5. 关联查询 JOIN
## INNER JOIN 内连接（两边都存在才查出）
```sql
SELECT u.uid,u.nickname
FROM t_room_member m
INNER JOIN t_user u ON m.uid = u.uid
WHERE m.room_id = 100;
```
作用：查询某个房间内所有成员的用户昵称

## LEFT JOIN 左连接（左表全部保留，右表匹配不到填NULL）

---
# 6. 常用函数（写SQL经常用到）
1. `NOW()` / `CURRENT_TIMESTAMP` 当前时间
2. `DATE_FORMAT(time,'%Y-%m-%d %H:%i:%s')` 格式化时间
3. `IFNULL(字段,默认值)` 如果字段为NULL，替换成默认值
```sql
SELECT IFNULL(to_uid,0) FROM t_chat_msg;
```

---
# 7. 权限、用户
```sql
-- 创建账号
CREATE USER 'chat_user'@'localhost' IDENTIFIED BY '123456';
-- 赋予数据库全部权限
GRANT ALL ON chat_room.* TO 'chat_user'@'localhost';
-- 刷新权限
FLUSH PRIVILEGES;
```

---
# 8. 高频避坑清单
1.  不要用 `SELECT *`；业务代码指定需要的字段
2.  `UPDATE / DELETE` 永远不要忘记 `WHERE`
3.  字符串必须单引号包裹；字段名用反引号
4.  存储表情统一使用 `utf8mb4`，不要utf8
5.  大表必须建立索引（消息表 `t_chat_msg`）
6.  重要操作使用事务，防止一半数据写入成功一半失败
7.  禁止明文存储密码，代码内加密后存入数据库

---
# 9. 分类记忆简单划分
- DDL（结构操作）：`CREATE / ALTER / DROP` 创建、修改、删除库/表
- DML（数据操作）：`INSERT SELECT UPDATE DELETE` 增查改删（业务90%在用）
- TCL（事务）：`START TRANSACTION / COMMIT / ROLLBACK`
- DCL（权限）：`CREATE USER GRANT`

