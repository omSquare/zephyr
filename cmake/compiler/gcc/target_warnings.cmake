# SPDX-License-Identifier: Apache-2.0

# See root CMakeLists.txt for description and expectations of these macros

macro(toolchain_cc_warning_base)

  zephyr_compile_options(
    -Wall
    -Wformat
    -Wformat-security
    -Wno-format-zero-length
    -Wno-main
  )

  zephyr_cc_option(-Wno-pointer-sign)

  # Prohibit void pointer arithmetic. Illegal in C99
  zephyr_cc_option(-Wpointer-arith)

endmacro()

macro(toolchain_cc_warning_dw_1)

  zephyr_compile_options(
    -Wextra
    -Wunused
    -Wno-unused-parameter
    -Wmissing-declarations
    -Wmissing-format-attribute
    -Wold-style-definition
    )
  zephyr_cc_option(
    -Wmissing-prototypes
    -Wmissing-include-dirs
    -Wunused-but-set-variable
    -Wno-missing-field-initializers
    )

endmacro()

macro(toolchain_cc_warning_dw_2)

  zephyr_compile_options(
    -Waggregate-return
    -Wcast-align
    -Wdisabled-optimization
    -Wnested-externs
    -Wshadow
    )
  zephyr_cc_option(
    -Wlogical-op
    -Wmissing-field-initializers
    )

endmacro()

macro(toolchain_cc_warning_dw_3)

  zephyr_compile_options(
    -Wbad-function-cast
    -Wcast-qual
    -Wconversion
    -Wpacked
    -Wpadded
    -Wpointer-arith
    -Wredundant-decls
    -Wswitch-default
    )
  zephyr_cc_option(
    -Wpacked-bitfield-compat
    -Wvla
    )

endmacro()

macro(toolchain_cc_warning_extended)

  zephyr_cc_option(
    -Wno-unused-but-set-variable
    )

endmacro()
