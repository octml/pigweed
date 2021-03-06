.. _module-pw_system:

=========
pw_system
=========
.. warning::
  Under construction, stay tuned!

GN Target Toolchain Template
============================
This module includes a target toolchain template called ``pw_system_target``
that reduces the amount of work required to declare a target toolchain with
pre-selected backends for pw_log, pw_assert, pw_malloc, pw_thread, and more.

.. code-block::

  # Declare a toolchain with suggested, compiler, compiler flags, and default
  # backends.
  pw_system_target("stm32f429i_disc1_stm32cube_size_optimized") {
    # These options drive the logic for automatic configuration by this
    # template.
    cpu = PW_SYSTEM_CPU.CORTEX_M4F
    scheduler = PW_SYSTEM_SCHEDULER.FREERTOS

    # The pre_init source set provides things like the interrupt vector table,
    # pre-main init, and provision of FreeRTOS hooks.
    link_deps = [ "$dir_pigweed/targets/stm32f429i_disc1_stm32cube:pre_init" ]

    # These are hardware-specific options that set up this particular board.
    # These are declared in ``declare_args()`` blocks throughout Pigweed. Any
    # build arguments set by the user will be overridden by these settings.
    build_args = {
      pw_third_party_freertos_CONFIG = "$dir_pigweed/targets/stm32f429i_disc1_stm32cube:stm32f4xx_freertos_config"
      pw_third_party_freertos_PORT = "$dir_pw_third_party/freertos:arm_cm4f"
      pw_sys_io_BACKEND = dir_pw_sys_io_stm32cube
      dir_pw_third_party_stm32cube = dir_pw_third_party_stm32cube_f4
      pw_third_party_stm32cube_PRODUCT = "STM32F429xx"
      pw_third_party_stm32cube_CONFIG =
          "//targets/stm32f429i_disc1_stm32cube:stm32f4xx_hal_config"
      pw_third_party_stm32cube_CORE_INIT = ""
      pw_boot_cortex_m_LINK_CONFIG_DEFINES = [
        "PW_BOOT_FLASH_BEGIN=0x08000200",
        "PW_BOOT_FLASH_SIZE=2048K",
        "PW_BOOT_HEAP_SIZE=7K",
        "PW_BOOT_MIN_STACK_SIZE=1K",
        "PW_BOOT_RAM_BEGIN=0x20000000",
        "PW_BOOT_RAM_SIZE=192K",
        "PW_BOOT_VECTOR_TABLE_BEGIN=0x08000000",
        "PW_BOOT_VECTOR_TABLE_SIZE=512",
      ]
    }
  }
