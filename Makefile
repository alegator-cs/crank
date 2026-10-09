.PHONY: all test list clean sanitize test-pcre2
all:
	python3 tools/build.py crank
test:
	python3 tools/test.py
list:
	python3 tools/build.py --list
sanitize:
	python3 tools/test.py --sanitize --quick
test-pcre2:
	python3 tools/test.py --pcre2
clean:
	rm -rf build
