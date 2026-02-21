build_lib_example(
  NAME tavrn-example
  SOURCE_FILES tavrn-example.cc
  LIBRARIES_TO_LINK
    ${libtavrn}
    ${libinternet}
    ${libwifi}
    ${libmobility}
    ${libapplications}
)
