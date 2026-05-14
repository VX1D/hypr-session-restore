PLUGIN_NAME = hypr-session-restore

SOURCE_FILES = $(wildcard src/*.cpp)
TEST_FILES = $(wildcard tests/*.cpp)
PKG_CFLAGS = $(shell pkg-config --cflags hyprland hyprutils hyprlang pixman-1 libdrm)

CXXFLAGS  += -std=c++26 -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
CXXFLAGS  += -fPIC -O2 -fstack-protector-strong -D_FORTIFY_SOURCE=3
CXXFLAGS  += $(PKG_CFLAGS)
LDFLAGS   += -shared -Wl,-z,relro,-z,now

TIDY_CHECKS = bugprone-*,cert-*,cppcoreguidelines-*,performance-*,portability-*,readability-*,-bugprone-narrowing-conversions,-bugprone-implicit-widening-of-multiplication-result,-cert-err33-c,-cppcoreguidelines-avoid-c-arrays,-cppcoreguidelines-pro-type-vararg,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-narrowing-conversions,-cppcoreguidelines-avoid-magic-numbers,-cppcoreguidelines-special-member-functions,-cppcoreguidelines-avoid-non-const-global-variables,-cppcoreguidelines-use-default-member-init,-readability-magic-numbers,-readability-identifier-length,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-readability-convert-member-functions-to-static,-readability-container-contains,-readability-function-cognitive-complexity,-readability-implicit-bool-conversion,-readability-qualified-auto,-readability-static-definition-in-anonymous-namespace,-readability-uppercase-literal-suffix,-portability-avoid-pragma-once
STRICT_TIDY_CHECKS = bugprone-*,cert-*,cppcoreguidelines-*,performance-*,portability-*,readability-*,-bugprone-narrowing-conversions,-cert-err33-c,-cppcoreguidelines-avoid-c-arrays,-cppcoreguidelines-pro-type-vararg,-cppcoreguidelines-pro-bounds-array-to-pointer-decay,-cppcoreguidelines-pro-bounds-avoid-unchecked-container-access,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-bounds-constant-array-index,-cppcoreguidelines-narrowing-conversions,-cppcoreguidelines-avoid-magic-numbers,-cppcoreguidelines-special-member-functions,-cppcoreguidelines-avoid-non-const-global-variables,-readability-magic-numbers,-readability-identifier-length,-readability-braces-around-statements,-readability-inconsistent-ifelse-braces,-readability-convert-member-functions-to-static,-readability-function-cognitive-complexity,-readability-implicit-bool-conversion,-readability-qualified-auto,-readability-static-definition-in-anonymous-namespace,-readability-uppercase-literal-suffix,-portability-avoid-pragma-once,-readability-container-contains,-bugprone-exception-escape,-bugprone-unchecked-optional-access

.PHONY: all clean install uninstall check tidy tidy-strict format-check test

all: $(PLUGIN_NAME).so

$(PLUGIN_NAME).so: $(SOURCE_FILES)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SOURCE_FILES)

clean:
	$(RM) $(PLUGIN_NAME).so $(TEST_BINS)

check: $(PLUGIN_NAME).so test tidy format-check
	readelf -lW $(PLUGIN_NAME).so | grep -q 'GNU_STACK.*RW '
	readelf -dW $(PLUGIN_NAME).so | grep -q 'BIND_NOW'

tidy:
	clang-tidy --quiet --checks='$(TIDY_CHECKS)' $(SOURCE_FILES) -- -std=c++26 $(PKG_CFLAGS)

tidy-strict:
	clang-tidy --quiet --checks='$(STRICT_TIDY_CHECKS)' $(SOURCE_FILES) tests/test_json.cpp -- -std=c++26 -I. $(PKG_CFLAGS)

format-check:
	clang-format --dry-run --Werror src/*.cpp src/*.hpp tests/*.cpp

TEST_BINS = tests/test_json tests/test_edge_cases

test: $(TEST_BINS)
	./tests/test_json
	./tests/test_edge_cases

tests/test_json: tests/test_json.cpp src/Json.hpp src/Snapshot.hpp src/SecureFile.hpp
	$(CXX) -std=c++26 -Wall -Wextra -Wpedantic -Werror -I. -o $@ tests/test_json.cpp

tests/test_edge_cases: tests/test_edge_cases.cpp src/Json.hpp src/Snapshot.hpp src/SecureFile.hpp
	$(CXX) -std=c++26 -Wall -Wextra -Wpedantic -Werror -I. -o $@ tests/test_edge_cases.cpp

install: $(PLUGIN_NAME).so
	install -Dm755 $(PLUGIN_NAME).so \
		$(DESTDIR)/usr/lib/hyprland/plugins/$(PLUGIN_NAME).so

uninstall:
	$(RM) $(DESTDIR)/usr/lib/hyprland/plugins/$(PLUGIN_NAME).so
