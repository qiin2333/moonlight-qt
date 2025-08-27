#!/bin/bash

# 编译所有翻译文件
for f in app/languages/*.ts; do
    echo "Processing $f..."
    /c/Qt/6.8.2/msvc2022_64/bin/lrelease "$f"
done

echo "Translation compilation completed!"
