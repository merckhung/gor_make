load("@rules_cc//cc:defs.bzl", "cc_library")

cc_library(
    name = "gormake",
    srcs = [
        "OSUnix.cc",
        "RdFile.cc",
        "WrFile.cc",
        "intrp.cc",
        "lexer.cc",
        "parser.cc",
    ],
    hdrs = [
        "OS.h",
        "RdFile.h",
        "WrFile.h",
        "ast.h",
        "gormake.h",
        "intrp.h",
        "lexer.h",
        "line.h",
        "macros.h",
        "parser.h",
        "table.h",
        "token.h",
    ],
    copts = ["-Wno-unused-parameter"],
    visibility = ["//visibility:public"],
)
