# TC397 Zephyr vMCU

Apollo 외부 Vehicle MCU의 Zephyr application과 out-of-tree DTS overlay다.
TC397 CPU0에서 Zephyr kernel, shell, UART monitor를 실행한다. Safety Island용
`../zephyr`와 `../zephyr_hsoc_src`는 별도 구성으로 유지한다.

## 빌드와 실행

이 저장소는 `cometzero/zephyr_vmcu_src`이며 Apollo workspace의
`hsoc-stack/components/system_mgmt/zephyrproject/zephyr_vmcu_src`에 submodule로 둔다.
아래 명령은 Apollo workspace root에서 실행한다.

```sh
# 최초: 고정된 Zephyr/GCC와 Python build dependency 준비
scripts/build/build_vmcu_zephyr.sh --bootstrap
# 이후 증분 firmware build
scripts/build/build_vmcu_zephyr.sh
# AP UART service가 포함된 BSP image
./yocto_build.sh --bsp
# TC397 Zephyr와 Apollo BSP를 같은 tmux 화면에서 실행
./run_qbox_yocto.sh --bsp
# 차량 CAN/CAN FD와 SIL Kit restbus 연결
scripts/build/build_vmcu_silkit.sh --bootstrap
./run_qbox_yocto.sh --bsp --sil-kit
```

기본 ELF는 `build/qbox-apollo-qvp/zephyr-vmcu/app/zephyr/zephyr.elf`다.
`--bootstrap`은 build 아래에 별도 source/toolchain/venv를 만들며 기존 SI source를
갱신하지 않는다. `west.yml`이 Zephyr revision을 고정하고 build script가 compiler
archive checksum을 검사한다. compiler는 별도 TriCore GCC 11.3.1이다. 이전
bare-metal probe의 GCC 9.4는 오래된 linker 때문에 이 Zephyr 빌드에 사용하지 않는다.
설정·compiler·ELF hash는 `app/vmcu-build.json`에 기록한다.

QEMU는 현재 workspace의 `KIT_AURIX_TC397B_TRB`, ASCLIN0/1/2, PORT0 GPIO와 M_CAN이 필요하다.
이전 UART-only binary를 사용하는 경우 현재 QEMU source를 다시 빌드한다.
자세한 경로와 검증은 [Zephyr 구현 문서](https://github.com/cometzero/project-apollo/blob/feature/qbox-rebase/doc/vmcu/zephyr-implementation.md)를 참조한다.

## UART와 shell

| UART | 역할 |
|---|---|
| ASCLIN0 `0xf0000600` | Apollo DW UART2 `0x301c0000`와 binary protocol 통신 |
| ASCLIN1 `0xf0000700` | `tc397` tmux pane의 Zephyr shell 및 상태 변화 알림 |
| ASCLIN2 `0xf0000800` | SI CL0 PL011 `0x2a820000`의 기존 PFDI 5초 보고 |
| PORT0 `0xf003a000` | SI PL061 `0x2a830000` 및 board reset의 독립 GPIO |

`tc397` pane을 선택해 다음 명령을 입력한다. F12는 실행 중인 QBox와 TC397을
함께 종료한다. `--no-vmcu`는 companion과 AP peer 없이 BSP를 실행한다.
AP peer는 launcher가 실행용 BSP 이미지에 전달한 `apollo.vmcu=1` boot argument가
있을 때만 자동 시작한다. 배포 원본 이미지는 변경하지 않는다.

```text
vmcu-cli status
vmcu-cli apollo ping
vmcu-cli apollo status
vmcu-cli heartbeat off
vmcu-cli heartbeat on
vmcu-cli safety status
vmcu-cli safety gpio
vmcu-cli gpio status
vmcu-cli gpio wake on
vmcu-cli gpio wake off
vmcu-cli monitor off
vmcu-cli monitor on
vmcu-cli can status
vmcu-cli pmic rail 0
vmcu-cli pmic fault 0
vmcu-cli recover status
vmcu-cli power status
```

`status`는 SI CL0 PFDI bitmap, 보고 sequence/age/interval과 GPIO edge count를 보여 준다.
`apollo ping/status`는 AP 관리 UART, `safety ping/status/gpio`는 SI Safety UART를
사용한다. `heartbeat off/on`은 SI의 5초 정기 보고를 진단 목적으로 중지/재개한다.
PFDI 자체와 SOC_ERROR toggle은 계속 동작한다. `workload`는 UNSUPPORTED다.

Zephyr thread가 SI 보고 15초 timeout과 SOC_ERROR 1.5초 edge watchdog을 검사한다.
정상 상태에서 주기적으로 콘솔을 출력하지 않으며 `monitor off/on`은 상태 변화
알림만 제어한다. `vmcu-cli gpio reset assert/release`는 AP reset 선을 실제로
변경하므로 실행 중인 AP가 reset된다. SI/PMIC/vMCU 전원은 유지한다.

상세 pin·주소·frame 계약과 한계는
[SI CL0 Safety Channel](https://github.com/cometzero/project-apollo/blob/feature/qbox-rebase/doc/vmcu/si0-safety-channel.md)을 참조한다.
CAN/CAN FD, PMIC 조회와 AP 복구·종료·wake는
[CAN과 제어 서비스](https://github.com/cometzero/project-apollo/blob/feature/qbox-rebase/doc/vmcu/can-and-services.md)를 참조한다.
PMIC rail sequencing, SC7와 SoC 전체 cold-off는 미지원이다.

## 파일 소유권

- `boards/qemu_tc3x.overlay`, `zephyr/module.yml`: upstream board에 UART 역할 추가.
- `prj.conf`, `src/main.c`: Zephyr shell, monitor thread, UART request/response.
- `include/`, `lib/`: 공유 codec, freshness/deadline 정책, AP RPC 처리.
- `drivers/can_tc397.c`, `src/vehicle_can.c`: TC397 M_CAN frontend, CAN 상태 보고와 제어.
- `tools/`: Apollo Linux용 `vmcu-ap`와 host 검사.
- `samples/bringup/`: kernel/shell/timer 기초 검증용 application.

root repository가 이 overlay를 소유한다. 빌드 source/dependency와 산출물은
`build/qbox-apollo-qvp/zephyr-vmcu/` 아래에만 생성한다.

## 라이선스

파일별 SPDX 식별자를 따른다. 애플리케이션·프로토콜은 GPL-2.0-or-later,
Zephyr 통합 설정·드라이버·bringup sample은 Apache-2.0이며 원문은 `LICENSES/`에 있다.
