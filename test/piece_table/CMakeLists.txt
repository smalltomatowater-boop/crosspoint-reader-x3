add_executable(PieceTableTest
  PieceTableTest.cpp
  ${REPO_ROOT}/lib/Editor/PieceTable.cpp
)

target_include_directories(PieceTableTest PRIVATE
  ${REPO_ROOT}/lib/Editor
)

target_link_libraries(PieceTableTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PieceTableTest)
