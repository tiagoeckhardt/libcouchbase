#!/bin/bash
cbc n1ql -u 475bb1f6e6c842ff -P af6304aae4ec2db9 -U https://cb-21460.couchbase.live:28091/information 'INSERT INTO information (key,value) VALUES ("emp_1234",{"empno": 1234,"ename": "COUNT","job": "MUPPET","mgr": 7902,"hiredate": "1989/10/15","sal": 900,"comm": 0,"dept": { "deptno": 20, "dname": "RESEARCH", "loc": "DALLAS" }}) RETURNING *'
