#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
NC='\033[0m'

HOME=$(pwd)
KERNEL_SRC="$HOME/linux-5.15.178"
IDNT_SRC="$HOME/identifier"
TRIG_SRC="$HOME/trigger"
LLVM_SRC="$HOME/llvm-project"

run_identifier() {
    echo -e "${BLUE}==> Starting identifier tool...${NC}"
    output_dir="$HOME/dump-5.15.178"
    mkdir -p "$output_dir"

    $IDNT_SRC/build/lib/identifier \
        -debug-verbose 0 \
        -dump-keystructs \
        -output-dir "$output_dir" \
        $(find "${KERNEL_SRC}" -name "*.c.bc") \
        2> $IDNT_SRC/err

    echo -e "${GREEN}Identifier tool execution completed.${NC}"

    echo -e "${BLUE}==> Merging output files...${NC}"
    cp "$HOME/tools/merge.py" "$output_dir"
    cd "$output_dir" || exit 1
    python3 merge.py $KERNEL_SRC

    merged_file="$output_dir/merged.json"
    if [ -f "$merged_file" ]; then
        echo -e "${GREEN}Output file has been successfully merged: $merged_file${NC}"
    else
        echo -e "${RED}Error: merged.json was not generated.${NC}"
        exit 1
    fi
}

run_trigger() {
    echo -e "${BLUE}==> Running trigger tool...${NC}"

    sed -i "s|/home/user/Tools/w2l/code/out/defdebug/dumpResults/merged_output.json|$HOME/dump/merged.json|g" "$HOME/llvm-caplog.patch"
    patch -p1 -d "$LLVM_SRC" < "$HOME/llvm-caplog.patch"

    cd $LLVM_SRC/build
    make && sudo make install
    if [ ! -f "/usr/local/lib/LLVMCapabilityLog.so" ]; then
        echo -e "${RED}Error: LLVMCapabilityLog.so not found. Aborting.${NC}"
        exit 1
    fi

    sudo mv /usr/local/bin/clang-14 /usr/local/bin/clang-14_bk
    sudo cp "$HOME/tools/clang-trigger" /usr/local/bin/clang-14

    cd "$KERNEL_SRC" || exit 1
    make clean
    make LLVM=1 -j"$(nproc)" bzImage 2> err

    cd "$TRIGG_SRC" || exit 1
    ./bin/syz-manager -config "$HOME/tools/default.cfg"

    echo -e "${GREEN}Trigger tool execution completed.${NC}"
}

show_help() {
    echo -e "${YELLOW}Usage: $0 <tool>${NC}"
    echo ""
    echo -e "${GREEN}Tools:${NC}"
    echo -e "  ${BLUE}identifier${NC}    Analyze the kernel source and identify specific objects."
    echo -e "  ${BLUE}trigger${NC}       Generate code to trigger specific kernel objects based on identifier output."
    exit 1
}

if [ $# -eq 0 ]; then
    echo -e "${RED}Error: No tool specified.${NC}"
    show_help
fi

case "$1" in
    identifier)
        run_identifier
        ;;
    trigger)
        run_trigger
        ;;
    *)
        echo -e "${RED}Error: Unknown tool '$1'.${NC}"
        show_help
        ;;
esac
