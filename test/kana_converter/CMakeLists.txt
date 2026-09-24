add_executable(KanaConverterTest
  KanaConverterTest.cpp
  ${REPO_ROOT}/lib/Editor/KanaConverter.cpp
  ${REPO_ROOT}/lib/Editor/SkkDictionary.cpp
)

target_include_directories(KanaConverterTest PRIVATE
  ${REPO_ROOT}/lib/Editor
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(KanaConverterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KanaConverterTest)
