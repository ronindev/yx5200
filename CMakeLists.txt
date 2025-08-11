project (yx5200)
add_library (${PROJECT_NAME} STATIC Src/yx5200.c)
target_include_directories(${PROJECT_NAME} PUBLIC Inc)