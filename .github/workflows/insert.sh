#!/bin/bash
cbq -u 475bb1f6e6c842ff -p af6304aae4ec2db9 -e "http://cb-21460.couchbase.live:28091" --script="'INSERT INTO CRUD_Lab (key,value) VALUES ("emp_1234",{"empno": 1234,"ename": "COUNT","job": "MUPPET","mgr": 7902,"hiredate": "1989/10/15","sal": 900,"comm": 0,"dept": { "deptno": 20, "dname": "RESEARCH", "loc": "DALLAS" }}) RETURNING *'"
