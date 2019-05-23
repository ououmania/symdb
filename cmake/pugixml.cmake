set(PUGIXML_LIBRARY_NAME pugixml)

set(PUGIXML_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pugixml)

add_subdirectory(${PUGIXML_ROOT_DIR}/ third_party/pugixml)
set(PUGIXML_LIBRARY ${PUGIXML_LIBRARY_NAME})
set(PUGIXML_INCLUDE_DIR "${PUGIXML_ROOT_DIR}/src")
