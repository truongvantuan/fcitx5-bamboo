file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/data/bamboo")
configure_file("${PROJECT_SOURCE_DIR}/data/vietnamese.cm.dict"
               "${CMAKE_CURRENT_BINARY_DIR}/data/bamboo/vietnamese.cm.dict"
               COPYONLY)
add_executable(testdirectinput testdirectinput.cpp ../src/bamboo.cpp)
target_include_directories(testdirectinput PRIVATE ../src "${PROJECT_BINARY_DIR}")
target_link_libraries(testdirectinput Fcitx5::Core Fcitx5::Config
                      Bamboo::Core Pthread::Pthread)
target_compile_definitions(testdirectinput PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}/data")
add_test(NAME directinput COMMAND testdirectinput)
