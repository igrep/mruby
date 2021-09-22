WASI_SDK_ROOT = ENV['WASI_SDK_ROOT'] || fail('Specify WASI_SDK_ROOT environment variable!')

MRuby::CrossBuild.new('32bit') do |conf|
  # load specific toolchain settings
  conf.toolchain :clang

  # Use mrbgems
  # conf.gem 'examples/mrbgems/ruby_extension_example'
  # conf.gem 'examples/mrbgems/c_extension_example' do |g|
  #   g.cc.flags << '-g' # append cflags in this gem
  # end
  # conf.gem 'examples/mrbgems/c_and_ruby_extension_example'
  # conf.gem :core => 'mruby-eval'
  # conf.gem :mgem => 'mruby-onig-regexp'
  # conf.gem :github => 'mattn/mruby-onig-regexp'
  # conf.gem :git => 'git@github.com:mattn/mruby-onig-regexp.git', :branch => 'master', :options => '-v'

  conf.gembox "stdlib"
  conf.gembox "stdlib-ext"
  # Use standard IO/File class
  conf.gem :core => "mruby-io"
  # Use standard print/puts/p
  conf.gem :core => "mruby-print"
  conf.gembox "math"
  conf.gembox "metaprog"

  # Generate mruby command
  conf.gem :core => "mruby-bin-mruby"

  #C compiler settings
  conf.cc do |cc|
    cc.command = "#{WASI_SDK_ROOT}/bin/clang"
    cc.flags = ["--sysroot=#{WASI_SDK_ROOT}/share/wasi-sysroot"]
    cc.include_paths = ["#{root}/include"]
    cc.defines = %w()
    cc.option_include_path = %q[-I"%s"]
    cc.option_define = '-D%s'
    cc.compile_options = %Q[%{flags} -MMD -o "%{outfile}" -c "%{infile}"]
  end

  conf.disable_cxx_exception

  conf.test_runner do |t|
    t.command = 'wasmtime'
  end

  # mrbc settings
  # conf.mrbc do |mrbc|
  #   mrbc.compile_options = "-g -B%{funcname} -o-" # The -g option is required for line numbers
  # end

  # Linker settings
  conf.linker do |linker|
    linker.command = "#{WASI_SDK_ROOT}/bin/wasm-ld"
    linker.flags = ['--verbose']
    linker.flags_before_libraries = []
    linker.libraries = %w(c clang_rt.builtins-wasm32)
    linker.flags_after_libraries = []
    linker.library_paths = ["#{WASI_SDK_ROOT}/share/wasi-sysroot/lib/wasm32-wasi", "#{WASI_SDK_ROOT}/lib/clang/11.0.0/lib/wasi/"]
    linker.option_library = '-l%s'
    linker.option_library_path = '-L%s'
    linker.link_options = "%{flags} -o '%{outfile}' '#{WASI_SDK_ROOT}/share/wasi-sysroot/lib/wasm32-wasi/crt1.o' %{objs} %{libs}"
  end

  # Archiver settings
  conf.archiver do |archiver|
    archiver.command = "#{WASI_SDK_ROOT}/bin/llvm-ar"
    archiver.archive_options = 'vrs "%{outfile}" %{objs}'
  end

  # Parser generator settings
  # conf.yacc do |yacc|
  #   yacc.command = ENV['YACC'] || 'bison'
  #   yacc.compile_options = %q[-o "%{outfile}" "%{infile}"]
  # end

  # gperf settings
  # conf.gperf do |gperf|
  #   gperf.command = 'gperf'
  #   gperf.compile_options = %q[-L ANSI-C -C -p -j1 -i 1 -g -o -t -N mrb_reserved_word -k"1,3,$" "%{infile}" > "%{outfile}"]
  # end

  # file extensions
  # conf.exts do |exts|
  #   exts.object = '.o'
  #   exts.executable = '' # '.exe' if Windows
  #   exts.library = '.a'
  # end

  # file separator
  # conf.file_separator = '/'

  # Turn on `enable_debug` for better debugging
  # conf.enable_debug
  conf.enable_bintest
  conf.enable_test
end
