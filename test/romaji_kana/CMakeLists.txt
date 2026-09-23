add_executable(RomajiKanaTest
  RomajiKanaTest.cpp
  ${REPO_ROOT}/lib/Editor/RomajiKana.cpp
)

target_include_directories(RomajiKanaTest PRIVATE
  ${REPO_ROOT}/lib/Editor
)

target_link_libraries(RomajiKanaTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RomajiKanaTest)
