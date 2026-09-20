/*
 * scan/busview.h —— 扫描控制器对总线的全部需求, 就这四个方法
 *
 * 控制器只认 BusView, 测试里换成 FakeBus 即可让状态机在无硬件下跑通。
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

   /* 冻在当前位置并保持保持力矩 (暂停/中止走它)。不写 6040h=0x0000, 那是卸力, 滑台会因自重下滑。 */
   virtual void     postStop() = 0;
};

/* 真总线: 四个转发。 */
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
