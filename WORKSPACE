workspace(name = "iouringcp")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "liburing",
    build_file = "//:third_party/liburing.BUILD",
    sha256 = "ca069ecc4aa1baf1031bd772e4e97f7e26dfb6bb733d79f70159589b22ab4dc0",
    strip_prefix = "liburing-liburing-2.0",
    urls = [
        "https://github.com/axboe/liburing/archive/refs/tags/liburing-2.0.tar.gz",
    ],
)