add_executable(test_db test_db.c)
enable_project_warnings(test_db)
target_link_libraries(test_db PRIVATE simpledb_lib)

add_test(NAME test_db COMMAND test_db)
