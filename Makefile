
UNAME_S := $(shell uname -s)
CDK_VERSION := cdk-5.0-20230201
ifeq ($(UNAME_S),Darwin)
CXXFLAGS ?= -Ilib/$(CDK_VERSION)/dist/include -Ilib/ncurses-6.3/dist/include -Ilib/protobuf-3.21.2/dist/include
LDLIBS ?= lib/protobuf-3.21.2/dist/lib/libprotobuf.a lib/$(CDK_VERSION)/dist/lib/libcdkw.a lib/ncurses-6.3/dist/lib/libncursesw_g.a
endif

ifeq ($(UNAME_S),Linux)
# If in Kochiku, use the system ncurses, since 6.3 is not compatible and 5.9
# does not compile without patches.
ifdef KOCHIKU_ENV
CXXFLAGS ?= -Ilib/$(CDK_VERSION)/dist/include -Ilib/protobuf-3.21.2/dist/include
LDLIBS ?= lib/protobuf-3.21.2/dist/lib/libprotobuf.a lib/cdk-5.0-20230201/dist/lib/libcdkw.a -lncursesw -lstdc++fs -lpthread
else
CXXFLAGS ?= -Ilib/$(CDK_VERSION)/dist/include -Ilib/ncurses-6.3/dist/include -Ilib/protobuf-3.21.2/dist/include
LDLIBS ?= lib/protobuf-3.21.2/dist/lib/libprotobuf.a lib/cdk-5.0-20230201/dist/lib/libcdkw.a lib/ncurses-6.3/dist/lib/libncursesw_g.a -lstdc++fs -lpthread
endif
endif



RpcExplorer: RpcExplorer.cc lib/.compile
	g++ -std=c++17 -o RpcExplorer RpcExplorer.cc $(CXXFLAGS) $(LDFLAGS) $(LDLIBS)

MWE: MWE.cc
	g++ -std=c++17 -o MWE MWE.cc  -Ilib/ncurses-6.3/dist/include  lib/ncurses-6.3/dist/lib/libncursesw_g.a

lib/.compile:
	rm -rf lib/
	CDK_VERSION="$(CDK_VERSION)" ./build_dependencies.sh
	touch $@

clean:
	rm -f RpcExplorer
