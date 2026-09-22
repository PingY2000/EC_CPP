#include "ophircom.h"

/* windows.h 会往整个翻译单元里泼 min/max 宏和一堆用不上的声明, 所以这边要收干净 */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>

#include <QHash>

#include <vector>

namespace scan {

namespace {

QString fromBstr(const BSTR b)
{
   if (!b)
      return QString();
   return QString::fromWCharArray(reinterpret_cast<const wchar_t *>(b), (int)SysStringLen(b));
}

BSTR toBstr(const QString &s)
{
   return SysAllocStringLen(reinterpret_cast<const wchar_t *>(s.utf16()), (UINT)s.size());
}

QString clsidToText(const CLSID &c)
{
   wchar_t buf[64] = {0};
   if (StringFromGUID2(c, buf, 64) <= 0)
      return QString();
   return QString::fromWCharArray(buf);
}

}   /* namespace */

namespace {

/* 一次方法调用的实参, 按手册里的声明顺序 push (p0 先 push, 然后 p1、p2……)。
 * rgvarg 的规矩是反的 (rgvarg[0] 是最后一个参数), 倒序在 fill() 里统一做。
 * 顺序错了两参方法会回 0x80020005 (DISP_E_TYPEMISMATCH), 单参方法照样通过。 */
class Args
{
public:
   ~Args() { for (BSTR b : m_owned) SysFreeString(b); }

   Args &inLong(long v)
   {
      VARIANT a; VariantInit(&a);
      a.vt = VT_I4; a.lVal = (LONG)v;
      m_decl.push_back(a);
      return *this;
   }

   Args &inText(const QString &s)
   {
      VARIANT a; VariantInit(&a);
      a.vt = VT_BSTR; a.bstrVal = toBstr(s);
      m_owned.push_back(a.bstrVal);      /* 本对象所有, 析构时释放 */
      m_decl.push_back(a);
      return *this;
   }

   Args &outLong(long *p)
   {
      VARIANT a; VariantInit(&a);
      a.vt = VT_BYREF | VT_I4; a.plVal = (LONG *)p;
      m_decl.push_back(a);
      return *this;
   }

   Args &outText(BSTR *p)
   {
      VARIANT a; VariantInit(&a);
      a.vt = VT_BYREF | VT_BSTR; a.pbstrVal = p;
      m_decl.push_back(a);
      return *this;
   }

   Args &outVariant(VARIANT *p)
   {
      VARIANT a; VariantInit(&a);
      a.vt = VT_BYREF | VT_VARIANT; a.pvarVal = p;
      m_decl.push_back(a);
      return *this;
   }

   Args &outBool(VARIANT_BOOL *p)
   {
      VARIANT a; VariantInit(&a);
      a.vt = VT_BYREF | VT_BOOL; a.pboolVal = p;
      m_decl.push_back(a);
      return *this;
   }

   void fill(DISPPARAMS *dp)
   {
      const int n = (int)m_decl.size();
      m_rev.resize((size_t)n);
      for (int i = 0; i < n; i++)
         m_rev[(size_t)(n - 1 - i)] = m_decl[(size_t)i];

      dp->rgvarg            = n ? m_rev.data() : nullptr;
      dp->rgdispidNamedArgs = nullptr;
      dp->cNamedArgs        = 0;
      dp->cArgs             = (UINT)n;
   }

private:
   QVector<VARIANT> m_decl;    /* 声明顺序 */
   QVector<VARIANT> m_rev;     /* 倒过来给 rgvarg */
   QVector<BSTR>    m_owned;   /* 自己分配的 BSTR */
};

}   /* namespace */

QString OphirCom::errorText(long hresult)
{
   /* 手册 "Error Codes" 表。中文是译文, 括号里留原始码 */
   struct Row { long code; const char *text; };
   static const Row kTable[] = {
      { (long)0x00000000, "没有错误" },
      { (long)0x80004001, "功能未实现" },
      { (long)0x80070057, "参数无效" },
      { (long)0x80004005, "未指明的失败" },
      { (long)0x80040200, "设备未打开" },
      { (long)0x80040201, "设备已经打开了" },
      { (long)0x80040202, "驱动安装失败" },
      { (long)0x80040203, "缺少文件" },
      { (long)0x80040300, "设备故障" },
      { (long)0x80040301, "设备固件版本不匹配" },
      { (long)0x80040302, "探头故障" },
      { (long)0x80040303, "探头固件版本不匹配" },
      { (long)0x80040304, "设备句柄无效" },
      { (long)0x80040305, "探头通道号无效" },
      { (long)0x80040306, "该型号探头不支持" },
      { (long)0x80040307, "该设备不支持此功能" },
      { (long)0x80040308, "设备已断开 (USB 已拔出)" },
      { (long)0x80040400, "写入探头失败" },
      { (long)0x80040401, "参数错误" },
      { (long)0x80040402, "建安全数组失败" },
      { (long)0x80040403, "该探头不适用" },
      { (long)0x80040404, "数值超出范围" },
      { (long)0x80040405, "命令失败" },
      { (long)0x80040500, "流模式尚未开始" },
      { (long)0x80040501, "已有通道处于流模式" },
   };

   const long c = (long)(quint32)hresult;
   for (const Row &r : kTable)
   {
      if (r.code == c)
         return QStringLiteral("%1 (0x%2)")
            .arg(QString::fromUtf8(r.text),
                 QString::number((quint32)c, 16).toUpper().rightJustified(8, QLatin1Char('0')));
   }

   return QStringLiteral("未知错误 (0x%1)")
      .arg(QString::number((quint32)c, 16).toUpper().rightJustified(8, QLatin1Char('0')));
}

/* 手册 "GetData Status Codes": 每一个返回项都要看 status。手册原话 "The status code must
 * always be checked to determine the meaning of each returned value" */
QString OphirCom::statusText(int status)
{
   switch (status)
   {
   case 0x000000: return QStringLiteral("有效");
   case 0x000001: return QStringLiteral("过量程");
   case 0x000002: return QStringLiteral("饱和");
   case 0x000003: return QStringLiteral("丢失");
   case 0x000004: return QStringLiteral("能量复位");
   case 0x000005: return QStringLiteral("等待中");
   case 0x000006: return QStringLiteral("累积中");
   case 0x000007: return QStringLiteral("超时");
   case 0x000008: return QStringLiteral("峰值过量程");
   case 0x000009: return QStringLiteral("能量过量程");
   case 0x040001: return QStringLiteral("滤片状态变化");
   case 0x050000: return QStringLiteral("脉冲频率");
   case 0x100000: return QStringLiteral("温度");
   case 0x200000: return QStringLiteral("过热告警");
   case 0x300000: return QStringLiteral("脉宽");
   case 0x400000: return QStringLiteral("峰值功率能量");
   default: break;
   }

   /* BeamTrack 那三组 (X / Y / 光斑尺寸)。PD300R 走不到这儿, 但仍分出来 */
   switch (status & 0xFFFF0000)
   {
   case 0x010000: return (status & 1) ? QStringLiteral("X 测量错误") : QStringLiteral("X 测量有效");
   case 0x020000: return (status & 1) ? QStringLiteral("Y 测量错误") : QStringLiteral("Y 测量有效");
   case 0x030000:
      if (status == 0x030002) return QStringLiteral("光斑尺寸告警");
      return (status & 1) ? QStringLiteral("光斑尺寸错误") : QStringLiteral("光斑尺寸有效");
   default: break;
   }

   return QStringLiteral("状态 %1 (保留值)").arg(status);
}

/* 前向声明: Impl::getOptionList 要用下面 "数组解码" 里的 readStringArray */
namespace { bool readStringArray(const VARIANT &v, QStringList *out, QString *err); }

namespace {

QString makeError(HRESULT hr, const EXCEPINFO &ei)
{
   /* 真正的错误码在 EXCEPINFO.scode 里, hr 只是 DISP_E_EXCEPTION (0x80020009) */
   const long code = ei.scode ? (long)ei.scode : (long)hr;
   QString s = OphirCom::errorText(code);

   const QString dev = fromBstr(ei.bstrDescription);
   if (!dev.isEmpty() && !s.contains(dev))
      s += QStringLiteral(" —— 设备返回: ") + dev;
   return s;
}

/* 不打 qWarning: 诊断输出都走界面上的 hint() 或 selftest 的 printf */

}   /* namespace */

struct OphirCom::Impl
{
   IDispatch *disp = nullptr;
   ITypeInfo *ti   = nullptr;
   ITypeLib  *tl   = nullptr;
   QHash<QString, DISPID> ids;

   /* 名字 → DISPID。用从 dll 资源加载的 ITypeInfo, 不走 IDispatch::GetIDsOfNames */
   DISPID idOf(const char *name, QString *err)
   {
      const QString key = QString::fromLatin1(name);
      const auto it = ids.constFind(key);
      if (it != ids.constEnd())
         return it.value();

      LPOLESTR nm = const_cast<LPOLESTR>(reinterpret_cast<const wchar_t *>(key.utf16()));
      MEMBERID mid = MEMBERID_NIL;
      const HRESULT hr = ti ? ti->GetIDsOfNames(&nm, 1, &mid) : E_FAIL;
      if (FAILED(hr) || mid == MEMBERID_NIL)
      {
         if (err)
            *err = QStringLiteral("该版本的 Ophir COM 对象不支持方法 %1 (0x%2)")
                      .arg(key, QString::number((quint32)hr, 16).toUpper());
         return MEMBERID_NIL;
      }

      ids.insert(key, mid);
      return mid;
   }

   /* Get / Set 成对方法的公共实现。是成员而不是自由函数: Impl 是私有的, 外面够不着 */
   bool getOptionList(const char *method, long h_device, long channel,
                      long *index, QStringList *options, QString *err)
   {
      long idx = -1;
      VARIANT opts; VariantInit(&opts);

      Args a;
      a.inLong(h_device);
      a.inLong(channel);
      a.outLong(&idx);
      a.outVariant(&opts);

      QString e;
      const bool ok = invoke(method, a, &e);
      if (ok)
      {
         QString e2;
         if (!readStringArray(opts, options, &e2))
         {
            if (err)
               *err = QStringLiteral("读取 %1 返回的选项表失败: %2")
                         .arg(QString::fromLatin1(method), e2);
            VariantClear(&opts);
            return false;
         }
         *index = idx;
      }
      else if (err)
         *err = e;

      VariantClear(&opts);
      return ok;
   }

   bool setIndex(const char *method, long h_device, long channel, long index, QString *err)
   {
      Args a;
      a.inLong(h_device);
      a.inLong(channel);
      a.inLong(index);

      QString e;
      if (!invoke(method, a, &e)) { if (err) *err = e; return false; }
      return true;
   }

   /* 调一个方法。args 按声明顺序给好了 */
   bool invoke(const char *name, Args &args, QString *err)
   {
      const DISPID id = idOf(name, err);
      if (id == MEMBERID_NIL)
         return false;

      DISPPARAMS dp;
      args.fill(&dp);

      VARIANT res; VariantInit(&res);
      EXCEPINFO ei; ZeroMemory(&ei, sizeof(ei));
      UINT argerr = 0;

      /* 用 ITypeInfo::Invoke 而非 IDispatch::Invoke: 后者内部也查注册表, 一样报
       * TYPE_E_LIBNOTREGISTERED (同 ophircom.h 里那条坏注册项) */
      const HRESULT hr = ti->Invoke(disp, id, DISPATCH_METHOD, &dp, &res, &ei, &argerr);

      VariantClear(&res);
      const bool ok = SUCCEEDED(hr);
      if (!ok && err)
         *err = QStringLiteral("%1: %2").arg(QString::fromLatin1(name), makeError(hr, ei));

      if (ei.bstrSource)       SysFreeString(ei.bstrSource);
      if (ei.bstrDescription)  SysFreeString(ei.bstrDescription);
      if (ei.bstrHelpFile)     SysFreeString(ei.bstrHelpFile);
      return ok;
   }
};

namespace {

/* ProgID → CLSID。注册表里这条是好的 */
bool progIdToClsid(CLSID *out)
{
   return SUCCEEDED(CLSIDFromProgID(L"OphirLMMeasurement.CoLMMeasurement", out));
}

/* CLSID → 服务端 dll 的路径。不指定 WOW64 视图: 64 / 32 位进程各取各自注册的那份 dll */
QString serverPathOf(const CLSID &clsid)
{
   const QString sub = QStringLiteral("CLSID\\%1\\InprocServer32").arg(clsidToText(clsid));

   wchar_t buf[1024] = {0};
   DWORD n = sizeof(buf);
   const LSTATUS st = RegGetValueW(HKEY_CLASSES_ROOT,
                                   reinterpret_cast<LPCWSTR>(sub.utf16()),
                                   nullptr, RRF_RT_REG_SZ, nullptr, buf, &n);
   if (st != ERROR_SUCCESS)
      return QString();
   return QString::fromWCharArray(buf);
}

/* 从 dll 的**资源**里读 typelib, 再挑出接口的 ITypeInfo:
 * 优先 ICoLMMeasurement2 (StarLab 3.50 / COM 对象 10.0 起), 退回老接口 */
bool loadInterfaceTypeInfo(const CLSID &clsid, const QString &dll,
                           ITypeLib **out_tl, ITypeInfo **out_ti, IDispatch *disp,
                           QString *err)
{
   ITypeLib *tl = nullptr;
   HRESULT hr = LoadTypeLibEx(reinterpret_cast<const wchar_t *>(dll.utf16()),
                              REGKIND_NONE, &tl);
   if (FAILED(hr) || !tl)
   {
      if (err)
         *err = QStringLiteral("从 %1 读取类型库失败: %2")
                   .arg(dll, OphirCom::errorText((long)hr));
      return false;
   }

   /* 按名字挑, 不写死 IID */
   ITypeInfo *ti = nullptr;
   for (const wchar_t *want : {L"ICoLMMeasurement2", L"ICoLMMeasurement"})
   {
      for (UINT i = 0; i < tl->GetTypeInfoCount(); i++)
      {
         ITypeInfo *cand = nullptr;
         if (FAILED(tl->GetTypeInfo(i, &cand)) || !cand)
            continue;

         BSTR nm = nullptr;
         cand->GetDocumentation(-1, &nm, nullptr, nullptr, nullptr);
         const bool hit = nm && wcscmp(nm, want) == 0;
         if (nm)
            SysFreeString(nm);

         if (!hit)
         {
            cand->Release();
            continue;
         }

         /* 类型库里有这个接口, 不等于手上这个对象实现了它: QI 一下, 把这件事变成一句
          * 明确的错, 而不是后面某次调用莫名失败 */
         TYPEATTR *ta = nullptr;
         if (SUCCEEDED(cand->GetTypeAttr(&ta)) && ta)
         {
            void *p = nullptr;
            const bool implemented = SUCCEEDED(disp->QueryInterface(ta->guid, &p));
            if (p)
               static_cast<IUnknown *>(p)->Release();
            cand->ReleaseTypeAttr(ta);

            if (!implemented)
            {
               cand->Release();
               continue;
            }
         }
         ti = cand;
         break;
      }
      if (ti)
         break;
   }

   if (!ti)
   {
      tl->Release();
      if (err)
         *err = QStringLiteral("%1 中含类型库, 但不含 ICoLMMeasurement 接口, StarLab 版本可能过低。")
                   .arg(dll);
      (void)clsid;
      return false;
   }

   *out_tl = tl;
   *out_ti = ti;
   return true;
}

}   /* namespace */

namespace {

/* GetData 回的三个 VARIANT 各装一个数组。手册说元素是 double / double / LONG, 但类型库
 * 那一层是 VARIANT: VT_ARRAY|VT_R8 与 VT_ARRAY|VT_VARIANT 两种都认, 认不出来就报错。 */
bool arrayDims(const VARIANT &v, LONG *lb, LONG *ub, VARTYPE *elem, QString *err,
               const char *what)
{
   if (v.vt == VT_EMPTY || v.vt == VT_NULL)
   {
      *lb = 0; *ub = -1; *elem = VT_EMPTY;
      return true;                     /* 没有数据: 正常, 不是错 */
   }

   if (!(v.vt & VT_ARRAY) || !v.parray)
   {
      if (err)
         *err = QStringLiteral("%1 不是数组 (vt=0x%2)")
                   .arg(QString::fromLatin1(what),
                        QString::number(v.vt, 16).toUpper().rightJustified(4, QLatin1Char('0')));
      return false;
   }

   if (SafeArrayGetDim(v.parray) != 1)
   {
      if (err)
         *err = QStringLiteral("%1 不是一维数组").arg(QString::fromLatin1(what));
      return false;
   }

   if (FAILED(SafeArrayGetLBound(v.parray, 1, lb)) ||
       FAILED(SafeArrayGetUBound(v.parray, 1, ub)))
   {
      if (err)
         *err = QStringLiteral("%1 无法取得数组上下界").arg(QString::fromLatin1(what));
      return false;
   }

   VARTYPE vt = VT_EMPTY;
   if (FAILED(SafeArrayGetVartype(v.parray, &vt)))
      vt = VT_VARIANT;
   *elem = vt;
   return true;
}

bool readDoubleArray(const VARIANT &v, QVector<double> *out, QString *err)
{
   out->clear();

   LONG lb = 0, ub = -1; VARTYPE elem = VT_EMPTY;
   if (!arrayDims(v, &lb, &ub, &elem, err, "value/timestamp"))
      return false;

   for (LONG i = lb; i <= ub; i++)
   {
      if (elem == VT_VARIANT)
      {
         VARIANT t; VariantInit(&t);
         const HRESULT hr = SafeArrayGetElement(v.parray, &i, &t);
         if (FAILED(hr)) { VariantClear(&t); if (err) *err = QStringLiteral("读取数组元素失败"); return false; }
         /* 元素也可能是别的数值类型, 让 OLE 去做转换 */
         VARIANT d; VariantInit(&d);
         const HRESULT c = VariantChangeType(&d, &t, 0, VT_R8);
         VariantClear(&t);
         if (FAILED(c)) { VariantClear(&d); if (err) *err = QStringLiteral("数组元素不是数值"); return false; }
         out->push_back(d.dblVal);
         VariantClear(&d);
      }
      else if (elem == VT_R8)
      {
         double d = 0.0;
         if (FAILED(SafeArrayGetElement(v.parray, &i, &d)))
         { if (err) *err = QStringLiteral("读取数组元素失败"); return false; }
         out->push_back(d);
      }
      else if (elem == VT_I4)
      {
         LONG l = 0;
         if (FAILED(SafeArrayGetElement(v.parray, &i, &l)))
         { if (err) *err = QStringLiteral("读取数组元素失败"); return false; }
         out->push_back((double)l);
      }
      else
      {
         if (err)
            *err = QStringLiteral("数组元素类型未知 (vt=%1)").arg((int)elem);
         return false;
      }
   }
   return true;
}

bool readLongArray(const VARIANT &v, QVector<int> *out, QString *err)
{
   out->clear();

   LONG lb = 0, ub = -1; VARTYPE elem = VT_EMPTY;
   if (!arrayDims(v, &lb, &ub, &elem, err, "status"))
      return false;

   for (LONG i = lb; i <= ub; i++)
   {
      if (elem == VT_VARIANT)
      {
         VARIANT t; VariantInit(&t);
         if (FAILED(SafeArrayGetElement(v.parray, &i, &t)))
         { VariantClear(&t); if (err) *err = QStringLiteral("读取数组元素失败"); return false; }
         VARIANT d; VariantInit(&d);
         const HRESULT c = VariantChangeType(&d, &t, 0, VT_I4);
         VariantClear(&t);
         if (FAILED(c)) { VariantClear(&d); if (err) *err = QStringLiteral("状态不是整数"); return false; }
         out->push_back((int)d.lVal);
         VariantClear(&d);
      }
      else if (elem == VT_I4)
      {
         LONG l = 0;
         if (FAILED(SafeArrayGetElement(v.parray, &i, &l)))
         { if (err) *err = QStringLiteral("读取数组元素失败"); return false; }
         out->push_back((int)l);
      }
      else
      {
         if (err)
            *err = QStringLiteral("状态数组元素类型未知 (vt=%1)").arg((int)elem);
         return false;
      }
   }
   return true;
}

/* [out] VARIANT 里装的一串 BSTR → QStringList。GetWavelengths/GetRanges 这些用它 */
bool readStringArray(const VARIANT &v, QStringList *out, QString *err)
{
   out->clear();

   LONG lb = 0, ub = -1; VARTYPE elem = VT_EMPTY;
   if (!arrayDims(v, &lb, &ub, &elem, err, "options"))
      return false;

   for (LONG i = lb; i <= ub; i++)
   {
      BSTR b = nullptr;
      if (FAILED(SafeArrayGetElement(v.parray, &i, &b)))
      {
         if (err) *err = QStringLiteral("读取选项失败");
         return false;
      }
      out->push_back(fromBstr(b));
      if (b)
         SysFreeString(b);
   }
   return true;
}

}   /* namespace */

OphirCom::OphirCom() = default;

OphirCom::~OphirCom()
{
   destroy();
}

bool OphirCom::isRegistered()
{
   CLSID c;
   return progIdToClsid(&c);
}

bool OphirCom::isAvailable()
{
   CLSID c;
   if (!progIdToClsid(&c))
      return false;

   const QString dll = serverPathOf(c);
   if (dll.isEmpty())
      return false;

   /* 能读出类型库才算能用: dll 在但资源读不出来一样调不了 */
   ITypeLib *tl = nullptr;
   const HRESULT hr = LoadTypeLibEx(reinterpret_cast<const wchar_t *>(dll.utf16()),
                                    REGKIND_NONE, &tl);
   if (tl)
      tl->Release();
   return SUCCEEDED(hr);
}

bool OphirCom::create(QString *err)
{
   destroy();
   d = new Impl;

   CLSID clsid;
   if (!progIdToClsid(&clsid))
   {
      if (err)
         *err = QStringLiteral("系统中未注册 OphirLMMeasurement.CoLMMeasurement。该 COM 对象由 StarLab 安装程序注册, "
                               "请确认 StarLab 已安装。");
      destroy();
      return false;
   }

   /* 服务端是 Apartment 模型: 创建它的线程必须已经 CoInitializeEx 过 (调用方负责) */
   HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IDispatch, (void **)&d->disp);
   if (FAILED(hr) || !d->disp)
   {
      if (err)
         *err = QStringLiteral("创建 Ophir COM 对象失败: %1").arg(errorText((long)hr));
      destroy();
      return false;
   }

   const QString dll = serverPathOf(clsid);
   if (dll.isEmpty())
   {
      if (err)
         *err = QStringLiteral("注册表 CLSID\\%1\\InprocServer32 中没有服务端 dll 路径")
                   .arg(clsidToText(clsid));
      destroy();
      return false;
   }

   QString e2;
   if (!loadInterfaceTypeInfo(clsid, dll, &d->tl, &d->ti, d->disp, &e2))
   {
      if (err)
         *err = e2;
      destroy();
      return false;
   }

   return true;
}

void OphirCom::destroy()
{
   if (!d)
      return;
   if (d->ti)   d->ti->Release();
   if (d->tl)   d->tl->Release();
   if (d->disp) d->disp->Release();
   delete d;
   d = nullptr;
}

bool OphirCom::isCreated() const
{
   return d && d->disp;
}

bool OphirCom::scanUsb(QStringList *serial_numbers, QString *err)
{
   serial_numbers->clear();
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   VARIANT out; VariantInit(&out);
   Args a;
   a.outVariant(&out);

   QString e;
   const bool ok = d->invoke("ScanUSB", a, &e);
   if (ok)
   {
      QString e2;
      if (!readStringArray(out, serial_numbers, &e2))
      {
         if (err) *err = QStringLiteral("读取 ScanUSB 返回值失败: %1").arg(e2);
         VariantClear(&out);
         return false;
      }
   }
   else if (err)
      *err = e;

   VariantClear(&out);
   return ok;
}

bool OphirCom::openUsbDevice(const QString &serial_number, long *h_device, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   long h = 0;
   Args a;
   a.inText(serial_number);      /* p0 按手册: [in] BSTR serialNumber */
   a.outLong(&h);                /* p1: [out] LONG* hDevice */

   QString e;
   if (!d->invoke("OpenUSBDevice", a, &e)) { if (err) *err = e; return false; }
   *h_device = h;
   return true;
}

bool OphirCom::closeDevice(long h_device, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   Args a;
   a.inLong(h_device);
   QString e;
   if (!d->invoke("Close", a, &e)) { if (err) *err = e; return false; }
   return true;
}

bool OphirCom::closeAll(QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   Args a;
   QString e;
   if (!d->invoke("CloseAll", a, &e)) { if (err) *err = e; return false; }
   return true;
}

bool OphirCom::isSensorExists(long h_device, long channel, bool *exists, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   VARIANT_BOOL b = VARIANT_FALSE;
   Args a;
   a.inLong(h_device);
   a.inLong(channel);
   a.outBool(&b);

   QString e;
   if (!d->invoke("IsSensorExists", a, &e)) { if (err) *err = e; return false; }
   *exists = (b != VARIANT_FALSE);
   return true;
}

bool OphirCom::getVersion(long *version, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   long v = 0;
   Args a;
   a.outLong(&v);
   QString e;
   if (!d->invoke("GetVersion", a, &e)) { if (err) *err = e; return false; }
   *version = v;
   return true;
}

bool OphirCom::getDriverVersion(QString *info, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   BSTR s = nullptr;
   Args a;
   a.outText(&s);
   QString e;
   const bool ok = d->invoke("GetDriverVersion", a, &e);
   if (ok)
      *info = fromBstr(s);
   else if (err)
      *err = e;
   if (s)
      SysFreeString(s);
   return ok;
}

bool OphirCom::getDeviceInfo(long h_device, DeviceInfo *out, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   BSTR name = nullptr, rom = nullptr, sn = nullptr;
   Args a;
   a.inLong(h_device);
   a.outText(&name);
   a.outText(&rom);
   a.outText(&sn);

   QString e;
   const bool ok = d->invoke("GetDeviceInfo", a, &e);
   if (ok)
   {
      out->name   = fromBstr(name);
      out->rom    = fromBstr(rom);
      out->serial = fromBstr(sn);
   }
   else if (err)
      *err = e;

   if (name) SysFreeString(name);
   if (rom)  SysFreeString(rom);
   if (sn)   SysFreeString(sn);
   return ok;
}

bool OphirCom::getSensorInfo(long h_device, long channel, SensorInfo *out, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   BSTR sn = nullptr, type = nullptr, name = nullptr;
   Args a;
   a.inLong(h_device);
   a.inLong(channel);
   a.outText(&sn);
   a.outText(&type);
   a.outText(&name);

   QString e;
   const bool ok = d->invoke("GetSensorInfo", a, &e);
   if (ok)
   {
      out->serial = fromBstr(sn);
      out->type   = fromBstr(type);
      out->name   = fromBstr(name);
   }
   else if (err)
      *err = e;

   if (sn)   SysFreeString(sn);
   if (type) SysFreeString(type);
   if (name) SysFreeString(name);
   return ok;
}

bool OphirCom::getWavelengths(long h, long ch, long *index, QStringList *options, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }
   return d->getOptionList("GetWavelengths", h, ch, index, options, err);
}

bool OphirCom::setWavelength(long h, long ch, long index, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }
   return d->setIndex("SetWavelength", h, ch, index, err);
}

bool OphirCom::addWavelength(long h, long ch, long wavelength, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   Args a;
   a.inLong(h);
   a.inLong(ch);
   a.inLong(wavelength);
   QString e;
   if (!d->invoke("AddWavelength", a, &e)) { if (err) *err = e; return false; }
   return true;
}

bool OphirCom::getRanges(long h, long ch, long *index, QStringList *options, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }
   return d->getOptionList("GetRanges", h, ch, index, options, err);
}

bool OphirCom::setRange(long h, long ch, long index, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }
   return d->setIndex("SetRange", h, ch, index, err);
}

bool OphirCom::getMeasurementMode(long h, long ch, long *index, QStringList *options, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }
   return d->getOptionList("GetMeasurementMode", h, ch, index, options, err);
}

bool OphirCom::setMeasurementMode(long h, long ch, long index, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }
   return d->setIndex("SetMeasurementMode", h, ch, index, err);
}

bool OphirCom::configureStreamMode(long h, long ch, long mode, long n_value, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   Args a;
   a.inLong(h);
   a.inLong(ch);
   a.inLong(mode);
   a.inLong(n_value);
   QString e;
   if (!d->invoke("ConfigureStreamMode", a, &e)) { if (err) *err = e; return false; }
   return true;
}

bool OphirCom::startStream(long h, long ch, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   Args a;
   a.inLong(h);
   a.inLong(ch);
   QString e;
   if (!d->invoke("StartStream", a, &e)) { if (err) *err = e; return false; }
   return true;
}

bool OphirCom::getData(long h, long ch, Data *out, QString *err)
{
   out->clear();
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   VARIANT v_val, v_ts, v_st;
   VariantInit(&v_val); VariantInit(&v_ts); VariantInit(&v_st);

   Args a;
   a.inLong(h);
   a.inLong(ch);
   a.outVariant(&v_val);
   a.outVariant(&v_ts);
   a.outVariant(&v_st);

   QString e;
   const bool ok = d->invoke("GetData", a, &e);

   if (ok)
   {
      /* 三个数组等长是手册的承诺; 不等长说明对不上, 宁可报错也不拿错位的数 */
      QString e1, e2, e3;
      QVector<double> vals, tss;
      const bool a1 = readDoubleArray(v_val, &vals, &e1);
      const bool a2 = a1 && readDoubleArray(v_ts, &tss, &e2);
      const bool a3 = a2 && readLongArray(v_st, &out->statuses, &e3);

      if (!a1 || !a2 || !a3)
      {
         if (err)
            *err = QStringLiteral("读取 GetData 返回值失败: %1").arg(!a1 ? e1 : (!a2 ? e2 : e3));
         out->clear();
      }
      else if (vals.size() != tss.size() || vals.size() != out->statuses.size())
      {
         if (err)
            *err = QStringLiteral("GetData 返回的三个数组长度不一致 (%1/%2/%3), 数据不可用。")
                      .arg(vals.size()).arg(tss.size()).arg(out->statuses.size());
         out->clear();
      }
      else
      {
         out->values     = vals;
         out->timestamps = tss;
      }

      VariantClear(&v_val); VariantClear(&v_ts); VariantClear(&v_st);
      return out->statuses.size() == out->values.size();
   }

   if (err)
      *err = e;
   VariantClear(&v_val); VariantClear(&v_ts); VariantClear(&v_st);
   return false;
}

bool OphirCom::stopStream(long h, long ch, QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   Args a;
   a.inLong(h);
   a.inLong(ch);
   QString e;
   if (!d->invoke("StopStream", a, &e)) { if (err) *err = e; return false; }
   return true;
}

bool OphirCom::stopAllStreams(QString *err)
{
   if (!isCreated()) { if (err) *err = QStringLiteral("COM 对象尚未创建"); return false; }

   Args a;
   QString e;
   if (!d->invoke("StopAllStreams", a, &e)) { if (err) *err = e; return false; }
   return true;
}

}   /* namespace scan */
