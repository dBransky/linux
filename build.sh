#!/bin/bash
NPROC=$(nproc)
MAKE_ARGS=(ARCH=x86_64 -j"$NPROC")
if command -v ccache &>/dev/null; then
    MAKE=(ccache make "${MAKE_ARGS[@]}")
else
    MAKE=(make "${MAKE_ARGS[@]}")
fi

run_make() {
    "${MAKE[@]}" "$@"
}

# if install was passed
if [ "$1" == "install" ]; then
    echo -e "\033[0;32mInstalling release kernel...\033[0m"
    sudo make modules_install
    sudo make install
    exit 0
fi
if [ "$1" == "release" ]; then
    echo -e "\033[0;32mBuilding release kernel...\033[0m"
    run_make
    if [ $? -ne 0 ]; then
        echo "Build failed"
        exit 1
    fi
    sudo make ARCH=x86_64 -j"$NPROC" modules_install
    sudo make install
    exit 0
fi
if [ "$1" == "modules" ]; then
    echo -e "\033[0;32mBuilding kernel modules...\033[0m"
    run_make modules
    exit $?
fi
if [ "$1" == "full" ]; then
    echo -e "\033[0;32mBuilding dev kernel (bzImage + modules)...\033[0m"
    run_make bzImage modules
    exit $?
fi

# Default: bzImage only (qemu_run.sh boots arch/x86/boot/bzImage)
echo -e "\033[0;32mBuilding dev kernel (bzImage)...\033[0m"
run_make bzImage
exit $?
