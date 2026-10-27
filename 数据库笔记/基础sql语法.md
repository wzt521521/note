1.select * from employees;
2.select name , department,salary from employees;
3.select name AS employee_name ,salary AS monthly_salary from employees;
4.select name ,salary from employees where salary>10000;
5.select name , department , salary from employees where department == "IT";
6.select name ,age from employees where age between 25 and 35 ;
7.select name ,department from employees where department in ("IT","Finance","Marketing");
8.select id , name from employees where name like '%Li%';
9.select distinct department from employees;
10.select name,salary ,age from employees where order by salary desc,age asc limit 5;

1.select id,name,email from employees where email is null;
2.select name ,department from employees where department != 'IT';
3.select name ,age,salary from employees where age <25 or salary >20000;
4.select name , department, status from employees where status = 'active' and department != 'Finance';
5.select count(*) from employees;
6.select avg(salary) from employees;
7.select MAX(salary) as max_salary ,MIN(salary) as min_salary from employees;
8.select sum(salary) from employees where department ='IT';
9.select count(*) from employees group_by(department);
10.select department ,avg(salary) as avg_salary from employees where status ='active' group by department having avg(salary)>12000;

1.select id,name,,email  from employees  where email is not null;
2.select name,salary,status from employees  where status ='active' and salary between 8000 and 15000;
3.select name ,hire_date  from employees  where hire_date > '2025-01-01';
4.select status , count(*) as employees_count from employees  group by status;
5.select MAX(salary) as max_salary ,department from employees group by department;
6.select department , count(*) as employees_count  from employees group by department having count(*) > 5;
7.select department , avg(salary) as avg_salary  from employees where status ='active' group by department ;
8.select name,salary,case when salary >=20000 then 'high' when salary>=10000 then 'medium' else 'low' end as salary_level from employees;
9.select department,sum(salary) as total_salary from employees group by department having sum(salary)>100000;
10.select department ,count(*) as employees_count,avg(salary)   as avg_salary  from employees where status ='active' group by department having count(*)>=3 and avg(salary) >12000 order by avg_salary desc;

1.select employees.name,department.department_name from employees join departments where employees.department_id = department.id;
2.select employees.name,salary,department.department_name from employees join departments where departments.department_name ='IT' and employees.department_id = department.id;
3.select employees.name ,departments.department_name from employees left join departments where employees.department_id = department.id;
4.select employees.name ,departments.department_name,departments.location from employees left join departments where employees.department_id = department.id;
5.select e.name ,d.department_name from employees as e join departments as d where e.department_id = d.id;
6.select d.department_name ,e.count(*) as employees_count from employees as e join departments as d where e.department_id = d.id group by e.department_id ;
7.select d.department_name,avg(e.salary) as avg_salary from employees as e join departments as d where e.department_id =d.id group by e.department_id;
8.select d.department_name , avg(e.salary) as avg_salary from employees as e join departments as d where e.department_id =d.id group by e.department_id having avg_salary >15000;
9.select d.department_name,e.count(*) as employees_count from employees as e left join departments as d where e.department_id =d.id group by e.department_id;
10.select d.department_name,e.count(*) as employees_count ,e.avg(salary) as avg_salary from employees as e join departments as d where e.department_id =d.id and e.status = 'active' group by e.department_id having employees_count >=3 and avg_salary >12000 order by avg_salary desc;
11.select d.department_name,count(d.id) as employees_count ,avg(e.salary) as avg_salary from employees as e join departments as d on e.department_id = d.id where status ='active' group by d.department_name having employees_count>=2 and avg_salary>10000 order by avg_salary desc;
12.select d.department_name ,count(d.id) as employees_count,max(e.salary) as max_salary from departments as d left join employees as e on e.department_id = d.id group by d.department_name;

1.select name ,salary from employees where salary > (select avg(salary) from employees);
2.select name ,salary from employees where salary = (select max(salary) from employees);
3.select e.name from employees as e join  departments as d on e.department_id = d.id where d.department_name ='IT';select name from employees where department_id = (select id from departments where department_name = 'IT');
4.select name ,salary from employees where department_id in (select id from departments where location = 'Singapore');
5.select name ,salary from employees where salary > (select avg(e.salary) as avg_salary from employees as e join departments as d on e.department_id = d.id where d.department_name = 'IT');
6.select department_name from departments where exits(select 1 from employees e where e.department_id =d.id);
7.select department_name from departments d where not exits (select 1 from employees e where e.department_id = d.id);
8.select name ,salary from employees e where status = 'active ' and salary >(select avg(salary) from employees where status ='active') order by salary desc;



delete负责删除行，例如
delete from employees where id=1;
delete from employees;//删除整张表所有数据（结构保留）
而drop多用于修改表的结构
alter table employees drop column name;//删除名字字段
ALTER TABLE projects DROP INDEX idx_project_code;//删除索引
