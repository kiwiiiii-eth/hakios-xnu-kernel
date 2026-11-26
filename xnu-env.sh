# source this (from bash) before building XNU on Linux
# Everything is found relative to this file. THEOS (default ~/theos) provides the cctools and the
# Darwin SDK; XNU_SRC is the kernel tree (xnu/apply.sh creates it).
DARWIN_REDWOOD=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export DARWIN_REDWOOD
export XNU_HOST_OS=$(uname)
# On macOS there is no theos: xnu-deps/setup.sh lays out xnu-deps/theos-mac like one (links to the
# cctools of Xcode and to Xcode's own iPhoneOS SDK).
if [ "$XNU_HOST_OS" = Darwin ]; then
	export THEOS=${THEOS:-$DARWIN_REDWOOD/xnu-deps/theos-mac}
else
	export THEOS=${THEOS:-$HOME/theos}
fi
export XNU_SRC=${XNU_SRC:-$DARWIN_REDWOOD/xnu-src/xnu-12377.121.6}
export XNUDEPS=$DARWIN_REDWOOD/xnu-deps
export PATH=$XNUDEPS/host/bin:$PATH
export XNU_SDK_PATH=$XNUDEPS/sdk/iPhoneOS26.5.xnu.sdk
export XNU_SDK_VERSION=26.5
# XNU 12377 takes its version from the System.kext of an SDK or a KDK, or from this (there is neither here): the Darwin of VERSION
export RC_DARWIN_KERNEL_VERSION=${RC_DARWIN_KERNEL_VERSION:-$(sed -n 's/^DARWIN *= *//p' "$DARWIN_REDWOOD/VERSION")}
export XNU_PLATFORM_PATH=$XNUDEPS/sdk/Platforms/iPhoneOS.platform
# XNU 12377 no longer carries three sets of headers (TrustCache, CoreEntitlements/V2, CodeSignature): they are in Apple's
# Kernel Debug Kit of the same release. xnu-deps/setup.sh keeps the headers of one here (none of its kernels or libraries).
export XNU_KDK=${XNU_KDK:-$XNUDEPS/kdk/KDK_26.5_25F71.kdk}
