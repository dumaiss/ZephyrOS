# The standalone SDK release version (work package 49c).
#
# `make sdk` names its tree and tarball build/kos-sdk-$(KOS_SDK_VERSION).
# The ABI number written into the SDK's VERSION file is read from
# formats/include/exe.h (EXE_ABI_VERSION), never copied here: the two
# drift apart silently if it is.
KOS_SDK_VERSION := 0.1.0
