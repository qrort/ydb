# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(4.2.1)

LICENSE(MIT)

PEERDIR(
    contrib/python/more-itertools
)

NO_LINT()

PY_SRCS(
    TOP_LEVEL
    jaraco/functools/__init__.py
    jaraco/functools/__init__.pyi
)

RESOURCE_FILES(
    PREFIX contrib/python/jaraco.functools/py3/
    .dist-info/METADATA
    .dist-info/top_level.txt
    jaraco/functools/py.typed
)

END()
