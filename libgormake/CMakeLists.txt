project(libgormake)

add_library(gormake STATIC
  lexer.cc
  parser.cc
  intrp.cc
  OSUnix.cc
  RdFile.cc
  WrFile.cc)
