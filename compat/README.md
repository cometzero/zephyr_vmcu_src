# 공유 Zephyr 4.1 TriCore 호환성

공유 원본은 `../zephyr` (`992228a26f3f`, Zephyr 4.1.0)이다. 이 버전에는
TriCore architecture가 없으므로 vMCU module의 arch/board/SoC roots로 제공한다.

## 원본과 라이선스

다음 디렉터리는 기존 vMCU가 사용하던 Zephyr revision
`894020886d647a5f9b8a58bdf2cb8c211a62716b`에서 가져왔다.
원본: https://github.com/zephyrproject-rtos/zephyr/tree/894020886d647a5f9b8a58bdf2cb8c211a62716b

- `arch/tricore`, `include/zephyr/arch/tricore`
- `boards/qemu/tricore`, `soc/qemu/tricore`, `dts/tricore/qemu/tc3x.dtsi`
- Infineon AURIX UART/interrupt/STM driver와 해당 DT bindings/header
- `cmake/target_tricore.cmake`

파일의 Infineon/Linumiz copyright와 Apache-2.0 SPDX를 보존했다.
`LICENSES/Apache-2.0.txt`에 원문이 있다. TC4x 파일은 원본의 공용 family 구성을
보존하기 위해 포함하지만 현재 Apollo 실행·검증 대상은 TC397/TC3x뿐이다.

## 4.1 변경 범위

- module Kconfig에서 TriCore 선택과 4.1의 thread-abort hook, flash offset을 연결한다.
- 초기화 함수는 4.1 `z_bss_zero`/`z_data_copy`, weak symbol은 assembler `.weak`를 쓴다.
- STM은 4.1 `drivers/timer/mips_cp0_timer.c`의 lock/announce 계약을 사용한다.
  64-bit counter와 tick remainder를 유지하며 지나간 equality comparator는 재설정한다.
  신규 generic timer framework 전체를 이식하지 않는다.
- CAN frontend는 4.1의 공개 `can_mcan.h`와 `CAN_MCAN_DATA_INITIALIZER`를 사용한다.
- `zephyr-4.1.patch`는 architecture dispatch, compiler/linker와 shell bitfield ABI만
  연결한다. shell bitfield 예약 영역과 field 순서는 위 원본 revision에서 가져왔다.
- patch는 recipe의 disposable source copy에만 적용한다. SI 빌드 source는 그대로다.

## 검증

공유 4.1 copy + 이 module로 CMake/Ninja 빌드 후 TC397 QEMU에서
`VMCU_INIT result=PASS`, `VMCU_CAN_INIT result=PASS`, `vmcu-cli status` 응답을 확인했다.
최종 Yocto 빌드·Apollo 연동 결과는 workspace `doc/vmcu/zephyr-implementation.md`를 따른다.
