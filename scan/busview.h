/*
 * scan/busview.h —— 扫描控制器对总线的**全部**需求, 就这四个方法
 *
 * 为什么要有这一层薄薄的接口而不是直接拿 EcatThread 用:
 *
 * [docs/hmi_click_position.md](../docs/hmi_click_position.md) 的「尚未验证」一节写得很清楚 ——
 * **hmi 的「连接」那条路一次都没在真机上跑过**。也就是说这个仓库目前**没有可用的真机回路**
 * 来验扫描状态机。而扫描状态机里真正会出错的是暂停/继续/中止、到位窗口、外部改目标、
 * 续扫从哪接上这些**时序**问题 —— 它们跟总线没关系, 只跟状态机有关。
 *
 * 所以控制器只认 BusView, 测试里换一个 FakeBus(自己按速度推 pos、能注入掉帧/故障/失能),
 * 整个状态机就能**在没有硬件的情况下跑通**。见 scan/selftest.cpp。
 *
 * 这层适配只有四行, 不改变 hmi 那边任何东西。
 */
#pragma once

#include <cstdint>

#include "ecatworker.h"

namespace scan {

class BusView
{
public:
   virtual ~BusView() = default;

   virtual BusTelem telemetry() const                    = 0;
   virtual void     setTarget(int axis, int32_t want_disp) = 0;
   virtual void     setSpeed (int axis, uint32_t vel)      = 0;

   /* 冻在当前位置、**保持保持力矩**。暂停与中止走的就是它 —— 语义与 hmi 的「停止」相同:
    * 故意不写 6040h=0x0000, 那是卸力, 滑台会因自重下滑。 */
   virtual void     postStop() = 0;
};

/* 真总线: 三个转发 + 一个转发。EcatThread 的接口本来就是这几个。 */
class EcatBusView : public BusView
{
public:
   explicit EcatBusView(EcatThread *t) : m_t(t) {}

   BusTelem telemetry() const override            { return m_t->telemetry(); }
   void setTarget(int axis, int32_t w) override   { m_t->setTarget(axis, w); }
   void setSpeed (int axis, uint32_t v) override  { m_t->setSpeed(axis, v); }
   void postStop() override                       { m_t->postStop(); }

private:
   EcatThread *m_t = nullptr;
};

}   /* namespace scan */
