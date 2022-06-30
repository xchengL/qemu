libwin32_srcs = files(
        'win32-utils.c',
)

libwin32 = static_library('win32-utils', libwin32_srcs + genh,
                        name_suffix: 'fa',
                        build_by_default: false)

win32utils = declare_dependency(link_whole: libwin32)
