set -eu
brw=$1
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cd "$scratch"
printf '旧.x旧.x\n' > input
test "$("$brw" search input '旧.')" = "$(printf '0\n5')"
"$brw" replace input '旧.' 新 result 2
printf '新x新x\n' > expected
cmp expected result
if "$brw" replace input x a wrong 3; then exit 1; fi
test ! -e wrong
if "$brw" replace input x a wrong -1; then exit 1; fi
test ! -e wrong
: > empty
"$brw" replace empty x y still-empty 0
cmp empty still-empty
if "$brw" search input ''; then exit 1; fi
printf 'a\000b' > binary
if "$brw" replace binary a b wrong; then exit 1; fi
test ! -e wrong
if "$brw" replace input x a result; then exit 1; fi
cmp expected result
"$brw" pack files.tar.gz input result
mkdir out
"$brw" unpack files.tar.gz out
cmp input out/input
cmp result out/result
if "$brw" pack missing.tar input missing; then exit 1; fi
test ! -e missing.tar
gzip -t files.tar.gz
cp input '资料'
"$brw" pack unicode.tar.gz '资料'
"$brw" unpack unicode.tar.gz out
cmp input 'out/资料'
printf 'existing file\n' > sentinel
cp sentinel out/input
if "$brw" unpack files.tar.gz out; then exit 1; fi
cmp sentinel out/input
mkdir nested
cp input nested/file
tar cf files.tar nested
"$brw" unpack files.tar out
cmp input out/nested/file
tar cf invalid.tar --transform='s|^input$|../outside|' input
if "$brw" unpack invalid.tar out; then exit 1; fi
test ! -e outside
