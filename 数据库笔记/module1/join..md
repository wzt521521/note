先准备测试数据（三张表简化成两张，更容易看懂）
employees 员工表

表格

| id | full_name |
| --- | --- |
| 1 | 张三 |
| 2 | 李四 |
| 3 | 王五 |

employee_projects 中间表（员工项目关联）

表格

| employee_id | project_id | role_name |
| --- | --- | --- |
| 1 | 101 | 后端 |
| 2 | 102 | 测试 |

> 
> 王五（id=3）**没有参加任何项目**

---

## 1. INNER JOIN（内连接）

```
SELECT e.id, e.full_name, ep.project_id, ep.role_name
FROM employees e
INNER JOIN employee_projects ep
ON e.id = ep.employee_id;
```

结果：只有两边匹配上的行才留下

表格

| id | full_name | project_id | role_name |
| --- | --- | --- | --- |
| 1 | 张三 | 101 | 后端 |
| 2 | 李四 | 102 | 测试 |

✅ 王五消失了，因为他在 ep 表里没有对应的记录。

---

## 2. LEFT JOIN（左连接，左表全部保留）

```
SELECT e.id, e.full_name, ep.project_id, ep.role_name
FROM employees e
LEFT JOIN employee_projects ep
ON e.id = ep.employee_id;
```

结果：左边 `employees` 的所有人都保留；匹配不到的地方填 NULL

表格

| id | full_name | project_id | role_name |
| --- | --- | --- | --- |
| 1 | 张三 | 101 | 后端 |
| 2 | 李四 | 102 | 测试 |
| 3 | 王五 | NULL | NULL |

✅ 王五还在，项目相关字段是 NULL。

---

## 一句话区分

- **INNER JOIN：取交集，必须两边都有匹配数据，才出结果**
- **LEFT JOIN：左表全部保留，能匹配上就带出右边数据，匹配不上右边是 NULL**

### 常用场景对照

SELECT
e.id,
e.employee_no,
e.full_name
FROM employees AS e
LEFT JOIN employee_projects AS ep
ON ep.employee_id = e.id
WHERE ep.employee_id IS NULL;

先from，然后join+on，然后再where，最后select