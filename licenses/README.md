# 许可全文与来源 / License Texts and Sources

这里集中保存第三方许可副本，便于源码和二进制交付时保留声明。原组件目录中的许可证与文件头未被替换。项目自有代码的 MIT 许可见 [../LICENSE](../LICENSE)，适用范围与未决来源见 [第三方清单](../THIRD_PARTY_NOTICES.md)。

This directory collects third-party license copies for source and binary distributions. It does not replace notices in the original component directories or file headers. The MIT License for original project-owned code is in [../LICENSE](../LICENSE); scope and unresolved provenance are documented in the [third-party inventory](../THIRD_PARTY_NOTICES.md).

`BSD-3-Clause-CMSIS-RTOS.txt` is transcribed from the local source header, removing comment prefixes only and converting its text encoding to UTF-8. The HAL text comes from the vendor's 1.1.10 tag. Python dependency licenses come from their pinned PyPI source distributions, verified against the SHA256 values published in PyPI metadata. GPLv3 accompanies LGPLv3 because the latter incorporates GPLv3 terms; its inclusion does not change the firmware's license.

`BSD-3-Clause-CMSIS-RTOS.txt` 从本地源码文件头提取，仅移除注释前缀并转为 UTF-8。HAL 许可来自供应商 1.1.10 标签。Python 依赖许可来自固定版本的 PyPI 源码包，并已核对 PyPI 公布的 SHA256。GPLv3 是 LGPLv3 引用的配套条款，不表示固件改用 GPL。

## 来源 / Sources

- [MIT-FreeRTOS.txt](MIT-FreeRTOS.txt)
  - Source: ../fw/cubemx/Middlewares/Third_Party/FreeRTOS/Source/LICENSE
  - File SHA256: `a74efb143c66527cdb7a96461fc4e9652cbd33700bd2a129a0407e31ae03084a`
- [Apache-2.0.txt](Apache-2.0.txt)
  - Source: ../fw/cubemx/Drivers/CMSIS/LICENSE.txt
  - File SHA256: `b40930bbcf80744c86c46a12bc9da056641d722716c378f5659b9e555ef833e1`
- [BSD-3-Clause-ST-HAL.txt](BSD-3-Clause-ST-HAL.txt)
  - Source: https://raw.githubusercontent.com/STMicroelectronics/stm32f1xx-hal-driver/v1.1.10/LICENSE.md
  - File SHA256: `dff179cfe1405dc3381dc6865bf4a5173b35605b65694d73356b01aaec1f09f1`
- [BSD-3-Clause-CMSIS-RTOS.txt](BSD-3-Clause-CMSIS-RTOS.txt)
  - Source: ../fw/cubemx/Middlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS/cmsis_os.h
  - File SHA256: `f6a080ee624fcf3ec4e5adf977b75496bced9e411c1d03d94efd94324798b89f`
- [LGPL-3.0-python-can.txt](LGPL-3.0-python-can.txt)
  - Source: https://pypi.org/project/python-can/4.6.1/ (sdist: python_can-4.6.1/LICENSE.txt; SHA256 290fea135d04b8504ebff33889cc6d301e2181a54099116609f940825ffe5005)
  - File SHA256: `da7eabb7bafdf7d3ae5e9f223aa5bdc1eece45ac569dc21b3b037520b4464768`
- [BSD-3-Clause-pyserial.txt](BSD-3-Clause-pyserial.txt)
  - Source: https://pypi.org/project/pyserial/3.5/ (sdist: pyserial-3.5/LICENSE.txt; SHA256 3c77e014170dfffbd816e6ffc205e9842efb10be9f58ec16d3e8675b4925cddb)
  - File SHA256: `f91cb9813de6a5b142b8f7f2dede630b5134160aedaeaf55f4d6a7e2593ca3f3`
- [GPL-3.0.txt](GPL-3.0.txt)
  - Source: https://www.gnu.org/licenses/gpl-3.0.txt
  - File SHA256: `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`

- [STM32CubeF1-V1.8.7-Package_license.md](STM32CubeF1-V1.8.7-Package_license.md)
  - Source: STM32Cube_FW_F1_V1.8.7/Package_license.md in the locally installed package selected by the project .ioc
  - File SHA256: `5d0016458729aeb4501e604bb8fa8385e1b258b0113af3c6915532cfb945817b`

License copies use LF line endings for stable Git storage; their legal wording is preserved. / 许可副本统一使用 LF 换行以便 Git 稳定保存，保留原始法律措辞。
