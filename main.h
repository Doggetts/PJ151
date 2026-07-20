#ifndef		__MAIN_H
#define		__MAIN_H

#include	"stc8h.h"
#include "stdio.h"
#include <intrins.h>
#include "math.h"

#define FOSC        24000000UL                              //系统时钟频率 24MHz
#define BRT         (65536 - FOSC / 115200 / 4)             //串口波特率115200

//========================================================================
//                              类型定义
//========================================================================

/* 任务调度组件 */
typedef struct 
{
	unsigned char Run;              //任务运行标志 1:待执行 0:空闲
	unsigned int TIMCount;          //任务倒计时计数器
	unsigned int TRITime;           //任务重载周期
	void (*TaskHook) (void);        //任务函数指针
} TASK_COMPONENTS;   

#define	COM_TX_Lenth	64          //串口发送缓冲区长度
#define	COM_RX_Lenth	64          //串口接收缓冲区长度
#define	TimeOutSet1		3           //串口帧超时时间(任务周期数)

/* 串口通信结构体 */
typedef struct
{ 
	unsigned char	busy;           //发送忙标志
	unsigned char RX_Cnt;           //接收字节计数
	unsigned char	RX_TimeOut;       //接收超时计数
	unsigned char TX_Buffer[COM_TX_Lenth];
	char RX_Buffer[COM_RX_Lenth];
}COMx_Define; 

/* 档位定义 */
#define GEAR_NONE   0               //无档位
#define GEAR_LEFT   1               //左档 P34
#define GEAR_MID    2               //中档 P32
#define GEAR_RIGHT  3               //右档 P33

/* 出光模式定义 */
#define MODE_OFF                0   //全部关闭
#define MODE_LASER_HOLD         1   //右档+绿键 短按 激光常亮
#define MODE_LASER_MOMENT       2   //右档+绿键 长按1s 激光瞬亮
#define MODE_LASER_WHITE_HOLD   3   //右档+绿键 双击 激光+白光常亮
#define MODE_LASER_WHITE_MOMENT 4   //右档+绿键 双击按住 激光+白光瞬亮
#define MODE_WHITE_HOLD         5   //右档+红键 短按 白光常亮
#define MODE_WHITE_MOMENT       6   //右档+红键 长按1s 白光瞬亮
#define MODE_WHITE_STROBE_HOLD  7   //右档+红键 双击 白光8Hz眩目常亮
#define MODE_WHITE_STROBE_MOMENT 8  //右档+红键 双击按住 白光8Hz眩目瞬亮
#define MODE_WHITE_DIM_HOLD     9   //右/左档+弱键 弱白光常亮
#define MODE_RED_MOMENT         10  //左档+红键 按住 红光瞬亮
#define MODE_RED_HOLD           11  //左档+红键 长按10s 红光常亮
#define MODE_GREEN_MOMENT       12  //左档+绿键 按住 绿光瞬亮
#define MODE_GREEN_HOLD         13  //左档+绿键 长按10s 绿光常亮

/* PWM占空比参数 */
#define PWM_ARR_MAX     100         //PWM周期计数值
#define PWM_DUTY_FULL   95          //强光占空比(约1.2V以内模拟调光)
#define PWM_DUTY_DIM    28          //弱光占空比

/* 按键时序参数(ms) */
#define KEY_DEBOUNCE_MS     40      //消抖时间
#define KEY_LONG1_MS        1000    //长按1s判定(瞬亮模式)
#define KEY_LONG10_MS       10000   //长按10s判定(红/绿光常亮锁定)
#define KEY_DBLCLICK_MS     400     //双击间隔窗口
#define KEY_DBLHOLD_MS      300     //双击第二下按住判定(瞬亮)

#define MID_SHUTDOWN_MS     5000    //中档保持5s后进入休眠

#define NTC_SUPPLY_V        5.0     //NTC分压电路供电电压(V)
#define NTC_R25             10000.0 //NTC标称阻值10K@25℃
#define NTC_R_PULL          10000.0 //NTC下端下拉电阻10K
#define NTC_B_VALUE         3950.0  //NTC B值
#define ADC_FULL_SCALE      1024.0  //ADC满量程(当前配置下10位有效,室温AD约512)

#define BAT_VOLT_ALARM      3.0f    //电池低压报警阈值(V)
#define BAT_VOLT_MODE_BLOCK 2.6f    //低于此电压禁止新开启出光模式(V)
#define BAT_VOLT_ALARM_CLR  3.1f    //报警恢复回滞(V)
#define BAT_VOLT_DEBOUNCE   2       //连续检测次数(500ms周期)

/* 系统运行数据结构体 */
typedef struct
{
	unsigned char Light_Mode;       //当前出光模式
	unsigned char Gear;             //当前档位
	unsigned char V5_On;            //5V升压使能状态

	unsigned char Out_White;        //白光输出 0:关 1:强 2:弱
	unsigned char Out_Green;        //绿光输出 0:关 1:开
	unsigned char Out_Red;          //红光输出 0:关 1:开
	unsigned char Out_Laser;        //激光输出 0:关 1:开
	unsigned char Strobe_On;        //8Hz爆闪使能
	unsigned char Moment_Active;    //瞬亮模式输出中标志

	unsigned int  Tick_ms;          //系统毫秒计数(1ms递增)
	unsigned char Strobe_Phase;     //爆闪相位 0/1

	float Board_Temp;               //NTC温度 单位:℃
	float Vref_Voltage;             //供电电压(电池电压) 单位:V

	unsigned char Ver_Major;        //固件主版本号
	unsigned char Ver_Minor;        //固件次版本号

	unsigned char Bat_Alarm;        //低压报警标志 1:报警中
	unsigned int  MidHold_ms;       //中档保持计时(ms)

}SYS_DATA_TYPE_STRUCT;

//========================================================================
//                            外部函数声明
//========================================================================

void Data_Init(void);
void AdcInit(void);
unsigned int ADCRead(unsigned char ChX);

void PWM_Init(void);
void PWM_SetWhite(unsigned char duty);
void PWM_SetGreen(unsigned char duty);
void PWM_SetRed(unsigned char duty);
void PWM_AllOff(void);

void Output_Apply(void);
void Output_AllOff(void);
void Mode_Set(unsigned char mode);
void Status_Report(void);

void UartInit(void);
void UartSend(char dat);
void UartSendStr(char *p);

void Timer0_Init(void);
void Task_Marks_Handler_Callback(void);
void Task_Pro_Handler_Callback(void);

void Hw_IO_Init(void);
void ShutDown(void);

#endif
