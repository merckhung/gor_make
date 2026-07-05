load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "gor_make",
    srcs = ["main.cc"],
    copts = ["-I$(GENDIR)/libgormake", "-Ilibgormake"],
    deps = ["//libgormake:gormake"],
)
