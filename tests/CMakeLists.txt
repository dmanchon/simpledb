add_executable(test_db test_db.c)
enable_project_warnings(test_db)
target_link_libraries(test_db PRIVATE simpledb_lib)

add_test(NAME test_db COMMAND test_db)

add_executable(test_exec_pipeline test_exec_pipeline.c)
enable_project_warnings(test_exec_pipeline)
target_link_libraries(test_exec_pipeline PRIVATE simpledb_lib)
add_test(NAME test_exec_pipeline COMMAND test_exec_pipeline)
