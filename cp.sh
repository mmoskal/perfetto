#!/bin/sh
cp -r ../perfetto/out/ui/ui/dist/* .
find . -name '*.map' | xargs rm
