# ODX 自动生成方案（基于 Vector 真模板）

## 素材（本地，不进 git）

`diag/_ref/`：
| 文件 | 来源 | 大小 |
|---|---|---|
| `template_UDS14.odx-d` | CANdelaStudio 用 `Vector_UDS_14.0.cddt` 导出 | 831 KB |
| `template_UDS14.cdd` | CANdelaStudio 原生工程 | 1.0 MB |

## 模板结构（实测）

```
<ODX MODEL-VERSION="2.2.0" xsi:noNamespaceSchemaLocation="odx.xsd">
  <DIAG-LAYER-CONTAINER>
    <BASE-VARIANT>
      <DIAG-DATA-DICTIONARY-SPEC>
        <DIAG-CODED-TYPES>   565 个
        <DATA-OBJECT-PROPS>  （DOP 引用 DIAG-CODED-TYPE-REF）
      </DIAG-DATA-DICTIONARY-SPEC>
      <REQUESTS>        91
      <POS-RESPONSES>   71
      <DIAG-SERVICES>   68
      <STATE-CHARTS>     3
      <ADMIN-DATA>       1
    </BASE-VARIANT>
  </DIAG-LAYER-CONTAINER>
</ODX>
```

## 与手写版的差异（此前 ODXStudio 报错的真正原因）

| 项 | 手写版 ✗ | 模板做法 ✓ |
|---|---|---|
| schema 声明 | 无 | `xsi:noNamespaceSchemaLocation="odx.xsd"` |
| DIAG-CODED-TYPE | 塞在 DOP 子元素 ✗ | 独立 `<DIAG-CODED-TYPES>` 段 + DOP 用 `DIAG-CODED-TYPE-REF` 引用 |
| 服务内容 | 只有 `REQUEST-REF` 空壳 ✗ | 完整 `REQUEST` / `POS-RESPONSE` + 每个 `PARAM` |
| ADMIN-DATA | 无 ✗ | 必须有 |
| SHORT-NAME | 含连字符 ✗ | `[A-Za-z][A-Za-z0-9_]*` |

## 生成器设计（待实现）

`tools/odx_build.py`：

1. 读 `diag/_ref/template_UDS14.odx-d`，保留 ODX 头、DLC、BASE-VARIANT 元信息、ADMIN-DATA、STATE-CHART
2. 从 `diag_table.json` 生成：DID 的 DOP/CODED-TYPE、例程、9 个服务的 REQUEST/POS-RESPONSE/PARAM
3. 丢弃模板里其余 59 个服务
4. 写 `diag/SBB_ECU.odx-d`

校验：`odxtools` 能 `add_odx_file` 载入 + ODXStudio 能无报错打开。

---

## 模板实测：真实 XML 形状（builder 的模具）

### 关键差异（手写版必挂的地方）

| 项 | 手写版 ✗ | Vector 实际 ✓ |
|---|---|---|
| xsi:type 大小写 | `StandardLengthType` ✗ | **`STANDARD-LENGTH-TYPE`**（全大写）|
| 参数类型 | 未用 | `CODED-CONST`、`TABLE-KEY`、`MATCHING-REQUEST-PARAM` 等 |
| DID 表示 | 常量 ✗ | **`TABLE-REF` 引用 TABLE**，TABLE 在 DICTIONARY 里 |
| REQUEST ID | 自定义 | `ID="_621"` 形式（下划线 + 数字）|

### 一个真实的 0x34 请求（模板里 Identifier Read，SID=0x22 的同构）

```xml
<REQUEST ID="_621">
  <SHORT-NAME>RQ_Identification_Read</SHORT-NAME>
  <LONG-NAME>RQ Identification Read</LONG-NAME>
  <PARAMS>
    <PARAM SEMANTIC="SERVICE-ID" xsi:type="CODED-CONST">
      <SHORT-NAME>SID_RQ</SHORT-NAME>
      <BYTE-POSITION>0</BYTE-POSITION>
      <CODED-VALUE>34</CODED-VALUE>
      <DIAG-CODED-TYPE BASE-DATA-TYPE="A_UINT32" xsi:type="STANDARD-LENGTH-TYPE">
        <BIT-LENGTH>8</BIT-LENGTH>
      </DIAG-CODED-TYPE>
    </PARAM>
    <PARAM SEMANTIC="ID" ID="_622" xsi:type="TABLE-KEY">
      <SHORT-NAME>RecordDataIdentifier</SHORT-NAME>
      <BYTE-POSITION>1</BYTE-POSITION>
      <TABLE-REF ID-REF="_136"/>
    </PARAM>
  </PARAMS>
</REQUEST>
```

### builder 要生成的对象（按依赖顺序）

```
1. DIAG-CODED-TYPES       每个长度一种（8/32/64/128 bit）—— 先建，后面都用 REF 引用
2. TABLES + TABLE-ROWS    DID 表、RID 表、NRC 表
3. DATA-OBJECT-PROPS      每个 DID 一个（引用 CODED-TYPE）
4. REQUESTS               10/11/22/27/31/34/36/37/3E 各一个
5. POS-RESPONSES          同上
6. DIAG-SERVICES          把 REQUEST + POS-RESPONSE + 负响应串起来
7. ADMIN-DATA / STATE-CHART  从模板原样保留
```

### 自检门禁

1. `odxtools.database.Database().add_odx_file(path)` 能加载
2. `xmllint --noout --schema odx.xsd`（若拿到 odx.xsd）
3. ODXStudio / CANdelaStudio 能无报错打开

---

## 坑记录：克隆服务必须重编号**所有** ID（含嵌套）

现象：把 0x11 的服务克隆成 0x10/0x3E 后，ODXStudio 报
`Unable to Resolve OdxLink`（几十条）。

原因：只改了 `DIAG-SERVICE` 和 `REQUEST` 的**顶层** `ID`。
`REQUEST` 内部的 `PARAM` 节点**也带自己的 `ID`**（模板里形如 `_622`），
克隆后两份完全相同的 ID 同时存在 → 767 个 ID 里出现重复 → 所有引用解析失败。

正确做法：
1. 克隆后遍历整棵子树，**每个带 `ID` 属性的节点都换成新 ID**；
2. 把"指向克隆内部"的 `ID-REF` 同步改成新 ID；
3. 只保留指向**共享对象**（POS-RESPONSE / TABLE / DOP）的 `ID-REF` 不变；
4. 导入前先自检：`ID` 全局唯一 + 所有 `ID-REF` 都能解析到目标。

这个自检可以脚本化（767 个 ID 扫一遍很快），应当加进 builder 的门禁里。
