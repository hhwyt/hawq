SET client_min_messages TO ERROR;
DROP SCHEMA IF EXISTS madlib_install_check_gpsql_newplda CASCADE;
CREATE SCHEMA madlib_install_check_gpsql_newplda;
SET search_path = madlib_install_check_gpsql_newplda, madlib;

---------------------------------------------------------------------------
-- Build vocabulary: 
---------------------------------------------------------------------------
CREATE TABLE plda_vocab(wordid INT4, word TEXT);

INSERT INTO plda_vocab VALUES
(0, 'code'), (1, 'data'), (2, 'graph'), (3, 'image'), (4, 'input'), (5,
'layer'), (6, 'learner'), (7, 'loss'), (8, 'model'), (9, 'network'), (10,
'neuron'), (11, 'object'), (12, 'output'), (13, 'rate'), (14, 'set'), (15,
'signal'), (16, 'sparse'), (17, 'spatial'), (18, 'system'), (19, 'training');

---------------------------------------------------------------------------
-- Build training dataset: 
---------------------------------------------------------------------------
CREATE TABLE plda_training 
(
    docid INT4, 
    wordid INT4, 
    count INT4
);

INSERT INTO plda_training VALUES
(0, 0, 2),(0, 3, 2),(0, 5, 1),(0, 7, 1),(0, 8, 1),(0, 9, 1),(0, 11, 1),(0, 13,
1), (1, 0, 1),(1, 3, 1),(1, 4, 1),(1, 5, 1),(1, 6, 1),(1, 7, 1),(1, 10, 1),(1,
14, 1),(1, 17, 1),(1, 18, 1), (2, 4, 2),(2, 5, 1),(2, 6, 2),(2, 12, 1),(2, 13,
1),(2, 15, 1),(2, 18, 2), (3, 0, 1),(3, 1, 2),(3, 12, 3),(3, 16, 1),(3, 17,
2),(3, 19, 1), (4, 1, 1),(4, 2, 1),(4, 3, 1),(4, 5, 1),(4, 6, 1),(4, 10, 1),(4,
11, 1),(4, 14, 1),(4, 18, 1),(4, 19, 1), (5, 0, 1),(5, 2, 1),(5, 5, 1),(5, 7,
1),(5, 10, 1),(5, 12, 1),(5, 16, 1),(5, 18, 1),(5, 19, 2), (6, 1, 1),(6, 3,
1),(6, 12, 2),(6, 13, 1),(6, 14, 2),(6, 15, 1),(6, 16, 1),(6, 17, 1), (7, 0,
1),(7, 2, 1),(7, 4, 1),(7, 5, 1),(7, 7, 2),(7, 8, 1),(7, 11, 1),(7, 14, 1),(7,
16, 1), (8, 2, 1),(8, 4, 4),(8, 6, 2),(8, 11, 1),(8, 15, 1),(8, 18, 1),
(9, 0, 1),(9, 1, 1),(9, 4, 1),(9, 9, 2),(9, 12, 2),(9, 15, 1),(9, 18, 1),(9,
19, 1);


CREATE TABLE plda_testing 
(
    docid INT4, 
    wordid INT4, 
    count INT4
);

INSERT INTO plda_testing VALUES
(0, 0, 2),(0, 8, 1),(0, 9, 1),(0, 10, 1),(0, 12, 1),(0, 15, 2),(0, 18, 1),(0,
19, 1), (1, 0, 1),(1, 2, 1),(1, 5, 1),(1, 7, 1),(1, 12, 2),(1, 13, 1),(1, 16,
1),(1, 17, 1),(1, 18, 1), (2, 0, 1),(2, 1, 1),(2, 2, 1),(2, 3, 1),(2, 4, 1),(2,
5, 1),(2, 6, 1),(2, 12, 1),(2, 14, 1),(2, 18, 1), (3, 2, 2),(3, 6, 2),(3, 7,
1),(3, 9, 1),(3, 11, 2),(3, 14, 1),(3, 15, 1), (4, 1, 1),(4, 2, 2),(4, 3,
1),(4, 5, 2),(4, 6, 1),(4, 11, 1),(4, 18, 2);

---------------------------------------------------------------------------
-- Test
---------------------------------------------------------------------------
SELECT MADLib.newplda_train(
    'plda_training', 
    'plda_model',
    'plda_output_data',
    20, 5, 2, 10, 0.01);

SELECT MADLib.newplda_predict(
    'plda_testing', 
    'plda_model', 
    'plda_pred');

SELECT MADLib.newplda_get_topic_desc(
    'plda_model', 
    'plda_vocab', 
    'topic_word_desc',
    5); 

SELECT MADLib.newplda_get_topic_word_count(
    'plda_model', 
    'topic_word_count'); 

SELECT MADLib.newplda_get_word_topic_count(
    'plda_model', 
    'topic_word_count'); 

SELECT MADLib.__newplda_util_index_sort(array[1, 4, 2, 3]); 
SELECT MADLib.__newplda_util_transpose(array[[1, 2, 3],[4, 5, 6]]); 
SELECT MADLib.__newplda_util_norm_with_smoothing(array[1, 4, 2, 3], 0.1); 

SELECT * 
FROM MADLib.__newplda_util_norm_vocab('plda_vocab', 'norm_plda_vocab');

SELECT * 
FROM MADLib.__newplda_util_norm_dataset('plda_testing', 'norm_plda_vocab',
'norm_plda_data');

SELECT * 
FROM MADLib.__newplda_util_conorm_data('plda_testing', 'plda_vocab',
'norm_plda_data', 'norm_plda_vocab');

DROP SCHEMA madlib_install_check_gpsql_newplda CASCADE;
