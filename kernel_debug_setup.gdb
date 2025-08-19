# GDB脚本用于内核调试设置

# 基本设置
set pagination off
set print pretty on
set print array on
set print array-indexes on

# 硬件断点设置
set breakpoint auto-hw on
set can-use-hw-watchpoints 1

# 内存访问设置
set mem inaccessible-by-default off
set trust-readonly-sections on

# 调试信息设置
set debug remote 0
set debug target 0

# 内核特定设置
set architecture i386:x86-64

# 定义一些有用的函数
define kernel-bt
    bt
    info registers
end

define kernel-info
    info threads
    info registers
    x/10i $pc
end

define safe-break
    if $argc == 1
        hbreak $arg0
    else
        echo Usage: safe-break function_name
    end
end

define kernel-symbols
    info functions start_kernel
    info functions kallsyms
    info functions schedule
end

# 打印帮助信息
define kernel-help
    echo Available kernel debugging commands:\n
    echo   kernel-bt     - Show backtrace and registers\n
    echo   kernel-info   - Show threads, registers, and disassembly\n
    echo   safe-break    - Set hardware breakpoint safely\n
    echo   kernel-symbols - Show important kernel symbols\n
    echo \n
    echo Hardware breakpoint commands:\n
    echo   hbreak function_name  - Set hardware breakpoint\n
    echo   hbreak *address       - Set hardware breakpoint at address\n
    echo \n
    echo Memory examination:\n
    echo   x/10i $pc            - Disassemble 10 instructions at PC\n
    echo   x/10x $rsp           - Show 10 hex values at stack pointer\n
end

echo Kernel debugging environment loaded.\n
echo Type 'kernel-help' for available commands.\n
