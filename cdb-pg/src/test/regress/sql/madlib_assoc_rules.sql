SET client_min_messages TO ERROR;
DROP SCHEMA IF EXISTS madlib_install_check_gpsql_assoc_rules CASCADE;
CREATE SCHEMA madlib_install_check_gpsql_assoc_rules;
SET search_path = madlib_install_check_gpsql_assoc_rules, madlib;

DROP TABLE IF EXISTS test_data1;
CREATE TABLE test_data1 (
    trans_id INT,
    product INT
);

DROP TABLE IF EXISTS test_data2;
CREATE TABLE test_data2 (
    trans_id INT
    , product VARCHAR
);

INSERT INTO test_data1 VALUES (1,1);
INSERT INTO test_data1 VALUES (1,2);
INSERT INTO test_data1 VALUES (3,3);
INSERT INTO test_data1 VALUES (8,4);
INSERT INTO test_data1 VALUES (10,1);
INSERT INTO test_data1 VALUES (10,2);
INSERT INTO test_data1 VALUES (10,3);
INSERT INTO test_data1 VALUES (19,2);

INSERT INTO test_data2 VALUES (1, 'beer');
INSERT INTO test_data2 VALUES (1, 'diapers');
INSERT INTO test_data2 VALUES (1, 'chips');
INSERT INTO test_data2 VALUES (2, 'beer');
INSERT INTO test_data2 VALUES (2, 'diapers');
INSERT INTO test_data2 VALUES (3, 'beer');
INSERT INTO test_data2 VALUES (3, 'diapers');
INSERT INTO test_data2 VALUES (4, 'beer');
INSERT INTO test_data2 VALUES (4, 'chips');
INSERT INTO test_data2 VALUES (5, 'beer');
INSERT INTO test_data2 VALUES (6, 'beer');
INSERT INTO test_data2 VALUES (6, 'diapers');
INSERT INTO test_data2 VALUES (6, 'chips');
INSERT INTO test_data2 VALUES (7, 'beer');
INSERT INTO test_data2 VALUES (7, 'diapers');

DROP TABLE IF EXISTS test1_exp_result;
CREATE TABLE test1_exp_result (
    ruleid integer,
    pre text[],
    post text[],
    support double precision,
    confidence double precision,
    lift double precision,
    conviction double precision
) ;

DROP TABLE IF EXISTS test2_exp_result;
CREATE TABLE test2_exp_result (
    ruleid integer,
    pre text[],
    post text[],
    support double precision,
    confidence double precision,
    lift double precision,
    conviction double precision
) ;


INSERT INTO test1_exp_result VALUES (7, '{3}', '{1}', 0.20000000000000001, 0.5, 1.2499999999999998, 1.2);
INSERT INTO test1_exp_result VALUES (4, '{2}', '{1}', 0.40000000000000002, 0.66666666666666674, 1.6666666666666667, 1.8000000000000003);
INSERT INTO test1_exp_result VALUES (1, '{1}', '{2,3}', 0.20000000000000001, 0.5, 2.4999999999999996, 1.6000000000000001);
INSERT INTO test1_exp_result VALUES (9, '{2,3}', '{1}', 0.20000000000000001, 1, 2.4999999999999996, 0);
INSERT INTO test1_exp_result VALUES (6, '{1,2}', '{3}', 0.20000000000000001, 0.5, 1.2499999999999998, 1.2);
INSERT INTO test1_exp_result VALUES (8, '{3}', '{2}', 0.20000000000000001, 0.5, 0.83333333333333337, 0.80000000000000004);
INSERT INTO test1_exp_result VALUES (5, '{1}', '{2}', 0.40000000000000002, 1, 1.6666666666666667, 0);
INSERT INTO test1_exp_result VALUES (2, '{3}', '{2,1}', 0.20000000000000001, 0.5, 1.2499999999999998, 1.2);
INSERT INTO test1_exp_result VALUES (10, '{3,1}', '{2}', 0.20000000000000001, 1, 1.6666666666666667, 0);
INSERT INTO test1_exp_result VALUES (3, '{1}', '{3}', 0.20000000000000001, 0.5, 1.2499999999999998, 1.2);

INSERT INTO test2_exp_result VALUES (7, '{chips,diapers}', '{beer}', 0.2857142857142857, 1, 1, 0);
INSERT INTO test2_exp_result VALUES (2, '{chips}', '{diapers}', 0.2857142857142857, 0.66666666666666663, 0.93333333333333324, 0.85714285714285698);
INSERT INTO test2_exp_result VALUES (1, '{chips}', '{diapers,beer}', 0.2857142857142857, 0.66666666666666663, 0.93333333333333324, 0.85714285714285698);
INSERT INTO test2_exp_result VALUES (6, '{diapers}', '{beer}', 0.7142857142857143, 1, 1, 0);
INSERT INTO test2_exp_result VALUES (4, '{beer}', '{diapers}', 0.7142857142857143, 0.7142857142857143, 1, 1);
INSERT INTO test2_exp_result VALUES (3, '{chips,beer}', '{diapers}', 0.2857142857142857, 0.66666666666666663, 0.93333333333333324, 0.85714285714285698);
INSERT INTO test2_exp_result VALUES (5, '{chips}', '{beer}', 0.42857142857142855, 1, 1, 0);

SELECT output_schema, output_table, total_rules 
FROM madlib.assoc_rules(
    .1, .5, 'trans_id', 'product',
    'test_data1','madlib_install_check_gpsql_assoc_rules', false); 

SELECT CASE WHEN count(*) = 10 then 'PASS' ELSE 'FAIL' END
FROM assoc_rules t1, test1_exp_result t2
WHERE madlib.__assoc_rules_array_eq(t1.pre, t2.pre) AND
      madlib.__assoc_rules_array_eq(t1.post, t2.post) AND
      abs(t1.support - t2.support) < 1E-10 AND
      abs(t1.confidence - t2.confidence) < 1E-10;

SELECT output_schema, output_table, total_rules 
FROM madlib.assoc_rules(
    .1, .5, 'trans_id', 'product',
    'test_data2','madlib_install_check_gpsql_assoc_rules', false); 

SELECT CASE WHEN count(*) = 7 then 'PASS' ELSE 'FAIL' END
FROM assoc_rules t1, test2_exp_result t2
WHERE madlib.__assoc_rules_array_eq(t1.pre, t2.pre) AND
      madlib.__assoc_rules_array_eq(t1.post, t2.post) AND
      abs(t1.support - t2.support) < 1E-10 AND
      abs(t1.confidence - t2.confidence) < 1E-10;

DROP TABLE IF EXISTS test_data1;
DROP TABLE IF EXISTS test_data2;
DROP TABLE IF EXISTS test2_exp_result;
DROP TABLE IF EXISTS test1_exp_result;

DROP SCHEMA madlib_install_check_gpsql_assoc_rules CASCADE;
