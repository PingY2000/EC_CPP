#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include "sm.h"

#define EC_TIMEOUTMON 500

char IOmap[4096];
int expectedWKC;
boolean needlf;
volatile int wkc;
boolean inOP;
uint8 currentgroup = 0;

// CiA 402 常用对象
#define INDEX_CONTROLWORD     0x6040
#define INDEX_STATUSWORD      0x6041
#define INDEX_MODE_OF_OP      0x6060
#define INDEX_TARGET_POS      0x607A
#define INDEX_ACTUAL_POS      0x6064
#define INDEX_MODE_DISPLAY    0x6061

// 控制字常用值
#define CW_SHUTDOWN           0x0006
#define CW_SWITCH_ON          0x0007
#define CW_ENABLE_OP          0x000F
#define CW_FAULT_RESET        0x0080

int main(int argc, char *argv[])
{
    int i, j, chk;
    uint16 controlword = 0;
    uint16 statusword = 0;
    int32  target_pos = 0;
    int32  actual_pos = 0;
    int8   mode = 8;          // 8 = CSP (Cyclic Synchronous Position)
    int    slave = 1;         // 默认第一个从站

    needlf = FALSE;
    inOP = FALSE;

    printf("SOEM YKD2205PE minimal example\n");

    if (argc < 2) {
        printf("Usage: %s ifname\n", argv[0]);
        return 1;
    }

    // 1. 初始化
    if (ec_init(argv[1])) {
        printf("ec_init on %s succeeded.\n", argv[1]);

        // 2. 扫描从站
        if (ec_config_init(FALSE) > 0) {
            printf("%d slaves found and configured.\n", ec_slavecount);

            // 打印从站信息
            for (i = 1; i <= ec_slavecount; i++) {
                printf("Slave %d: Name=%s, State=%d, Output size=%d, Input size=%d\n",
                       i, ec_slave[i].name, ec_slave[i].state,
                       ec_slave[i].Obits, ec_slave[i].Ibits);
            }

            // 3. 映射过程数据（使用从站默认 PDO）
            ec_config_map(&IOmap);
            ec_configdc();

            printf("Slaves mapped, state to SAFE_OP.\n");

            // 4. 请求 SAFE_OP
            ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);

            // 5. 通过 SDO 设置工作模式 = CSP (8)
            printf("Set Mode of Operation = CSP (8)\n");
            ecx_SDOwrite(&ecx_context, slave, INDEX_MODE_OF_OP, 0, FALSE,
                         sizeof(mode), &mode, EC_TIMEOUTRXM);

            // 6. 进入 OPERATIONAL
            printf("Request operational state for all slaves\n");
            expectedWKC = (ec_group[0].outputsWKC * 2) + ec_group[0].inputsWKC;
            printf("Calculated workcounter %d\n", expectedWKC);

            ec_slave[0].state = EC_STATE_OPERATIONAL;
            // 发送一次过程数据，让从站能进入 OP
            ec_send_processdata();
            ec_receive_processdata(EC_TIMEOUTRET);
            // 请求 OP
            ec_writestate(0);
            chk = 40;
            do {
                ec_send_processdata();
                ec_receive_processdata(EC_TIMEOUTRET);
                ec_statecheck(0, EC_STATE_OPERATIONAL, 50000);
            } while (chk-- && (ec_slave[0].state != EC_STATE_OPERATIONAL));

            if (ec_slave[0].state == EC_STATE_OPERATIONAL) {
                printf("Operational state reached for all slaves.\n");
                inOP = TRUE;

                // 7. 使能顺序（CiA 402 状态机）
                printf("Enable drive sequence...\n");

                // Shutdown
                controlword = CW_SHUTDOWN;
                // 用 SDO 写控制字，不依赖 RxPDO 偏移
                ecx_SDOwrite(&ecx_context, slave, INDEX_CONTROLWORD, 0, FALSE,
                             sizeof(controlword), &controlword, EC_TIMEOUTRXM);
                usleep(50000);

                // Switch on
                controlword = CW_SWITCH_ON;
                ecx_SDOwrite(&ecx_context, slave, INDEX_CONTROLWORD, 0, FALSE,
                             sizeof(controlword), &controlword, EC_TIMEOUTRXM);
                usleep(50000);

                // Enable operation
                controlword = CW_ENABLE_OP;
                ecx_SDOwrite(&ecx_context, slave, INDEX_CONTROLWORD, 0, FALSE,
                             sizeof(controlword), &controlword, EC_TIMEOUTRXM);
                usleep(100000);

                // 读取状态字确认
                ecx_SDOread(&ecx_context, slave, INDEX_STATUSWORD, 0, FALSE,
                            &j, &statusword, EC_TIMEOUTRXM);
                printf("Statusword after enable: 0x%04X\n", statusword);

                // 8. 周期通信循环（示例：慢慢移动位置）
                printf("Start cyclic communication (CSP mode)...\n");
                target_pos = 0;

                for (i = 0; i < 10000; i++) {   // 运行约 10 秒（1ms 周期）
                    // 发送过程数据
                    ec_send_processdata();
                    wkc = ec_receive_processdata(EC_TIMEOUTRET);

                    if (wkc >= expectedWKC) {
                        // 默认 RxPDO 偏移须用 SOEM slaveinfo/TwinCAT 确认
                        // （前 2B Controlword，后 4B Target Position）
                        // 用 SDO 更新目标位置，调试用，实时性差
                        if (i % 100 == 0) {  // 每 100ms 更新一次
                            target_pos += 1000;  // 每次增加 1000 脉冲
                            ecx_SDOwrite(&ecx_context, slave, INDEX_TARGET_POS, 0, FALSE,
                                         sizeof(target_pos), &target_pos, EC_TIMEOUTRXM);

                            ecx_SDOread(&ecx_context, slave, INDEX_ACTUAL_POS, 0, FALSE,
                                        &j, &actual_pos, EC_TIMEOUTRXM);
                            printf("Target=%d  Actual=%d  Status=0x%04X\n",
                                   target_pos, actual_pos, statusword);
                        }
                    } else {
                        printf("WKC error: %d / %d\n", wkc, expectedWKC);
                    }

                    usleep(1000);  // 1ms 周期
                }

                // 9. 关闭
                controlword = CW_SHUTDOWN;
                ecx_SDOwrite(&ecx_context, slave, INDEX_CONTROLWORD, 0, FALSE,
                             sizeof(controlword), &controlword, EC_TIMEOUTRXM);

                inOP = FALSE;
            } else {
                printf("Not all slaves reached operational state.\n");
                ec_readstate();
                for (i = 1; i <= ec_slavecount; i++) {
                    if (ec_slave[i].state != EC_STATE_OPERATIONAL) {
                        printf("Slave %d State=0x%2.2x StatusCode=0x%4.4x : %s\n",
                               i, ec_slave[i].state, ec_slave[i].ALstatuscode,
                               ec_ALstatuscode2string(ec_slave[i].ALstatuscode));
                    }
                }
            }

            printf("Request init state for all slaves\n");
            ec_slave[0].state = EC_STATE_INIT;
            ec_writestate(0);
        } else {
            printf("No slaves found!\n");
        }
        printf("End simple test, close socket\n");
        ec_close();
    } else {
        printf("No socket connection on %s\n", argv[1]);
    }
    return 0;
}