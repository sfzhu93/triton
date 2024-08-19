alias pp='https_proxy=http://fwdproxy:8080 http_proxy=http://fwdproxy:8080 no_proxy=.fbcdn.net,.facebook.com,.thefacebook.com,.tfbnw.net,.fb.com,.fburl.com,.facebook.net,.sb.fbsbx.com,localhost'

export TRITON_BUILD_DEBUG=1
export https_proxy="http://fwdproxy:8080"
export http_proxy="http://fwdproxy:8080"


DEBUG=$TRITON_BUILD_DEBUG TRITON_BUILD_WITH_CLANG_LLD=1 \
#   LLVM_INCLUDE_DIRS=$LLVM_BUILD_DIR/include LLVM_LIBRARY_DIR=$LLVM_BUILD_DIR/lib LLVM_SYSPATH=$LLVM_BUILD_DIR \
pip install -e python --no-build-isolation
