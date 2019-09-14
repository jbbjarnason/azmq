project(readme)

#find_package(nlohmann_json 3.2.0 REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} ${CMAKE_THREAD_LIBS_INIT}
                                      ${ZeroMQ_LIBRARIES}
                                      ${Boost_LIBRARIES})
#                                        PRIVATE nlohmann_json::nlohmann_json)

