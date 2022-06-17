qtest_ss = ss.source_set()
qtest_ss.add(files(
  'qtest.c',
))

qtest_module_ss.add_all(when: ['CONFIG_SOFTMMU', 'CONFIG_POSIX'],
                    if_true: qtest_ss)
qtest_module_ss.add_all(when: ['CONFIG_WIN32'], if_true: qtest_ss)
