//  methods.hpp - AES70/OCA
//  命名常量(DefLevel/MethodIndex/EventIndex/PduType/ClassID)
//
//  方法索引来源:ocac 仓库(派生自 AES70 XMI),用于
//  OcaRoot/OcaDeviceManager/OcaBlock。 EV2 订阅方法索引为候选值,需对照
//  AES70-2-2023 Annex A XMI 校验(见 Step 5)。

#ifndef OCA_METHODS_HPP_
#define OCA_METHODS_HPP_

#include <cstdint>

namespace oca::methods {

// Ocp1MessageType (AES70-3-2023 §9.1.3)
constexpr uint8_t kPduCommand = 0;     // Ocp1Command
constexpr uint8_t kPduCommandRrq = 1;  // Ocp1CommandRrq
constexpr uint8_t kPduNtf1 = 2;        // deprecated EV1
constexpr uint8_t kPduResponse = 3;    // Ocp1Response
constexpr uint8_t kPduKeepAlive = 4;   // Ocp1KeepAlive
constexpr uint8_t kPduNtf2 = 5;        // Ocp1Notification2 (EV2)

// Definition levels (ClassID 深度)
constexpr uint16_t kDefLevelRoot = 1;     // OcaRoot {1}
constexpr uint16_t kDefLevelManager = 2;  // OcaManager {1,3} / OcaWorker {1,1}
constexpr uint16_t kDefLevelDeviceMngr = 3;   // OcaDeviceManager {1,3,1}
constexpr uint16_t kDefLevelBlock = 3;        // OcaBlock {1,1,3}
constexpr uint16_t kDefLevelNetworkMngr = 3;  // OcaNetworkManager {1,3,6}
constexpr uint16_t kDefLevelSubMngr = 3;      // OcaSubscriptionManager {1,3,4}

// OcaRoot methods (DefLevel 1) - ocac 核对
constexpr uint16_t kRootGetClassIdentification = 1;
constexpr uint16_t kRootGetLockable = 2;
constexpr uint16_t kRootLock = 3;  // SetLockNoReadWrite
constexpr uint16_t kRootUnlock = 4;
constexpr uint16_t kRootGetRole = 5;
constexpr uint16_t kRootLockReadonly = 6;  // SetLockNoWrite

// OcaDeviceManager methods (DefLevel 3) - ocac 核对
constexpr uint16_t kDevGetOcaVersion = 1;
constexpr uint16_t kDevGetModelGUID = 2;
constexpr uint16_t kDevGetSerialNumber = 3;
constexpr uint16_t kDevGetDeviceName = 4;
constexpr uint16_t kDevSetDeviceName = 5;
constexpr uint16_t kDevGetModelDescription = 6;
constexpr uint16_t kDevGetEnabled = 11;           // OCAMicro
constexpr uint16_t kDevSetEnabled = 12;           // OCAMicro
constexpr uint16_t kDevGetDeviceRevisionID = 20;  // sphinx 3.20,deprecated v3
constexpr uint16_t kDevGetState =
    13;  // deprecated (v3), 被 GetOperationalState 取代
constexpr uint16_t kDevGetOperationalState = 23;  // sphinx 2024
constexpr uint16_t kDevGetManagers = 19;
constexpr uint16_t kDevGetManufacturer = 21;  // sphinx 3.21,2023 Mandatory G4
constexpr uint16_t kDevGetProduct = 22;       // sphinx 3.22,2023 Mandatory G3

// OcaBlock methods (DefLevel 3) - ocac 核对
constexpr uint16_t kBlockGetMembers = 5;
constexpr uint16_t kBlockGetMembersRecursive =
    6;  // AES70-2015 起,2018 起非强制;
        // 实装以让合规工具 GetObjects
        // 走非覆盖分支(见 Spec3 根因)

// OcaWorker methods (DefLevel 2, classID{1,1}) - OCAMicro OcaLiteWorker
// 阶段二工具未测对象 Worker 顺延项,Spec3 实装后 test4 对根块(OcaWorker 子类)
// 强制 GetEnabled/SetEnabled/GetPorts。最小返值合规(见设计文档 Spec3 节)。
constexpr uint16_t kWorkerGetEnabled = 1;  // OCAMicro
constexpr uint16_t kWorkerSetEnabled = 2;  // OCAMicro
constexpr uint16_t kWorkerGetPorts = 5;    // OCAMicro

// OcaAgent methods (DefLevel 2, classID{1,2}) - OCAMicro OcaLiteAgent
// OcaNetwork{1,2,1} 继承 OcaAgent{1,2},工具对 ONo 4097 测 Agent 强制方法
// (GetLabel/SetLabel/GetOwner/GetPath)。在 OcaAgent 中间类实装。
constexpr uint16_t kAgentGetLabel = 1;  // OCAMicro
constexpr uint16_t kAgentSetLabel = 2;  // OCAMicro
constexpr uint16_t kAgentGetOwner = 3;  // OCAMicro
constexpr uint16_t kAgentGetPath = 4;   // OCAMicro

// OcaWorker additional methods (DefLevel 2, classID{1,1}) - OCAMicro
// OcaLiteWorker
constexpr uint16_t kWorkerGetLabel = 8;   // OCAMicro
constexpr uint16_t kWorkerSetLabel = 9;   // OCAMicro
constexpr uint16_t kWorkerGetOwner = 10;  // OCAMicro
constexpr uint16_t kWorkerGetPath = 13;   // OCAMicro

// OcaApplicationNetwork methods (DefLevel 2, classID{1,4})
// OcaControlNetwork{1,4,1} 前缀匹配 OcaApplicationNetwork{1,4},工具对 4098 也测
// GetServiceID/GetSystemInterfaces(Mandatory=true)。在 OcaControlNetwork
// 实例上实装。
constexpr uint16_t kAppNetGetServiceID = 4;         // ReferenceOCCMembers
constexpr uint16_t kAppNetGetSystemInterfaces = 6;  // ReferenceOCCMembers
constexpr uint16_t kAppNetGetLabel = 1;             // OCAMicro
constexpr uint16_t kAppNetSetLabel = 2;             // OCAMicro
constexpr uint16_t kAppNetGetOwner = 3;             // OCAMicro
constexpr uint16_t kAppNetGetPath = 10;             // OCAMicro

// OcaNetwork methods (DefLevel 3, classID{1,2,1}) - OCAMicro OcaLiteNetwork
// DeprecatedSince AES70-2018 / 2023 进一步弃用;本实例仅为兼容 AES70-2018
// 合规工具的最小强制实例(见 Spec3 计划、设计文档 2023 弃用立场)。
constexpr uint16_t kNet2GetLinkType = 1;          // OCAMicro
constexpr uint16_t kNet2GetIDAdvertised = 2;      // OCAMicro
constexpr uint16_t kNet2GetControlProtocol = 4;   // OCAMicro
constexpr uint16_t kNet2GetMediaProtocol = 5;     // OCAMicro
constexpr uint16_t kNet2GetSystemInterfaces = 9;  // OCAMicro
constexpr uint16_t kNet2Shutdown =
    13;  // OCAMicro;XML 2018 Mandatory=false 但工具
         // 仍判 mandatory(日志坐实),实装以合规

// OcaControlNetwork methods (DefLevel 3, classID{1,4,1}) - AES70-2018 mandatory
// AvailableSince AES70-2018;无 DeviceType 门。唯一强制方法 GetControlProtocol。
constexpr uint16_t kCtrlNetGetControlProtocol = 1;

// OcaNetworkManager methods (DefLevel 3) - OCAMicro
constexpr uint16_t kNetGetNetworks = 1;                // OCAMicro
constexpr uint16_t kNetGetStreamNetworks = 2;          // OCAMicro
constexpr uint16_t kNetGetControlNetworks = 3;         // OCAMicro
constexpr uint16_t kNetGetMediaTransportNetworks = 4;  // OCAMicro

// OcaSubscriptionManager EV1 methods (DefLevel 3) - OCAMicro 校验(deprecated
// v3)
constexpr uint16_t kSubAddSubscription = 1;                   // OCAMicro 3.1
constexpr uint16_t kSubRemoveSubscription = 2;                // 3.2
constexpr uint16_t kSubAddPropertyChangeSubscription = 5;     // 3.5
constexpr uint16_t kSubRemovePropertyChangeSubscription = 6;  // 3.6

// OcaSubscriptionManager EV2 methods (DefLevel 3) - sphinx 2024 校验
constexpr uint16_t kSubAddSubscription2 = 8;                 // sphinx 2024 3.8
constexpr uint16_t kSubRemoveSubscription2 = 9;              // 3.9
constexpr uint16_t kSubAddPropertyChangeSubscription2 = 10;  // 3.10
constexpr uint16_t kSubRemovePropertyChangeSubscription2 = 11;  // 3.11

// OcaRoot events (DefLevel 1)
constexpr uint16_t kEventPropertyChanged = 1;

// OcaDeviceManager events (DefLevel 3)
constexpr uint16_t kEventOperationalState = 1;  // DeviceState 变化(演示事件)

// Spec4:PropertyChanged 通知负载中的 PropertyID.propertyIndex(AES70
// OcaPropertyID = {声明类 defLevel, 类属性表下标};propertyIndex 与 methodIndex
// 独立命名空间)。Label/Enabled 均为各类首个可报变属性。
constexpr uint16_t kPropLabel = 1;    // OcaAgent/OcaWorker/OcaAppNet 的 Label
constexpr uint16_t kPropEnabled = 1;  // OcaDeviceManager 的 Enabled

// ── OcaMediaClockManager methods (DefLevel 3, classID{1,3,7}) ──
constexpr uint16_t kDefLevelMediaClockMngr = 3;
constexpr uint16_t kMcmGetClocks = 1;                    // AES70-2018 Mandatory
constexpr uint16_t kMcmGetMediaClockTypesSupported = 2;  // AES70-2018 Mandatory
constexpr uint16_t kMcmGetClock3s = 3;                   // AES70-2018 Mandatory

// OcaMediaClockManager Property indices
constexpr uint16_t kMcmPropClockSourceTypesSupported = 1;
constexpr uint16_t kMcmPropClocks = 2;
constexpr uint16_t kMcmPropClock3s = 3;

// ── OcaMediaClock3 methods (DefLevel 3, classID{1,2,15}) ──
// AvailableSince AES70-2018,替代已废弃 OcaMediaClock{1,2,6}。
constexpr uint16_t kDefLevelMediaClock3 = 3;
constexpr uint16_t kMc3GetAvailability = 1;    // AES70-2018 Mandatory
constexpr uint16_t kMc3SetAvailability = 2;    // Optional
constexpr uint16_t kMc3GetCurrentRate = 3;     // AES70-2018 Mandatory
constexpr uint16_t kMc3SetCurrentRate = 4;     // Optional
constexpr uint16_t kMc3GetOffset = 5;          // AES70-2018 Mandatory
constexpr uint16_t kMc3SetOffset = 6;          // Optional
constexpr uint16_t kMc3GetSupportedRates = 7;  // AES70-2018 Mandatory
constexpr uint16_t kMc3GetPTPStatus = 0x8002;  // Fitcan 私有

// OcaMediaClock3 Property indices
constexpr uint16_t kMc3PropAvailability = 1;
constexpr uint16_t kMc3PropTimeSourceONo = 2;
constexpr uint16_t kMc3PropOffset = 3;
constexpr uint16_t kMc3PropCurrentRate = 4;

// ── OcaMediaTransportNetwork methods (DefLevel 3, classID{1,4,2}) ──
constexpr uint16_t kDefLevelMtn = 3;
constexpr uint16_t kMtnGetMediaProtocol = 1;             // AES70-2018 Mandatory
constexpr uint16_t kMtnGetPorts = 2;                     // AES70-2018 Mandatory
constexpr uint16_t kMtnGetPortName = 3;                  // Optional
constexpr uint16_t kMtnSetPortName = 4;                  // Optional
constexpr uint16_t kMtnGetMaxSourceConnectors = 5;       // AES70-2018 Mandatory
constexpr uint16_t kMtnGetMaxSinkConnectors = 6;         // AES70-2018 Mandatory
constexpr uint16_t kMtnGetMaxPinsPerConnector = 7;       // AES70-2018 Mandatory
constexpr uint16_t kMtnGetMaxPortsPerPin = 8;            // AES70-2018 Mandatory
constexpr uint16_t kMtnGetSourceConnectors = 9;          // Optional
constexpr uint16_t kMtnGetSourceConnector = 10;          // Optional
constexpr uint16_t kMtnGetSinkConnectors = 11;           // Optional
constexpr uint16_t kMtnGetSinkConnector = 12;            // Optional
constexpr uint16_t kMtnGetConnectorsStatuses = 13;       // AES70-2018 Mandatory
constexpr uint16_t kMtnGetConnectorStatus = 14;          // AES70-2018 Mandatory
constexpr uint16_t kMtnAddSourceConnector = 15;          // Optional
constexpr uint16_t kMtnAddSinkConnector = 16;            // Optional
constexpr uint16_t kMtnControlConnector = 17;            // Optional
constexpr uint16_t kMtnSetSourceConnectorPinMap = 18;    // Optional
constexpr uint16_t kMtnSetSinkConnectorPinMap = 19;      // Optional
constexpr uint16_t kMtnSetConnectorConnection = 20;      // Optional
constexpr uint16_t kMtnSetConnectorCoding = 21;          // Optional
constexpr uint16_t kMtnSetConnectorAlignmentLevel = 22;  // Optional
constexpr uint16_t kMtnSetConnectorAlignmentGain = 23;   // Optional
constexpr uint16_t kMtnDeleteConnector = 24;             // AES70-2018 Mandatory
constexpr uint16_t kMtnGetAlignmentLevel = 25;           // Optional
constexpr uint16_t kMtnGetAlignmentGain = 26;            // Optional

// OcaMediaTransportNetwork Property indices
constexpr uint16_t kMtnPropProtocol = 1;
constexpr uint16_t kMtnPropPorts = 2;
constexpr uint16_t kMtnPropMaxSourceConnectors = 3;
constexpr uint16_t kMtnPropMaxSinkConnectors = 4;
constexpr uint16_t kMtnPropMaxPinsPerConnector = 5;
constexpr uint16_t kMtnPropMaxPortsPerPin = 6;

// OcaMediaTransportNetwork Event indices
constexpr uint16_t kEventSourceConnectorChanged = 1;
constexpr uint16_t kEventSinkConnectorChanged = 2;
constexpr uint16_t kEventConnectorStatusChanged = 3;

// ── OcaMediaTransportNetworkAES67 methods (DefLevel 7) ──
// ClassID {1,4,2,0xFFFF,0xFA,0x2EE9,1}, defLevel = fieldCount = 7
constexpr uint16_t kDefLevelMtnAes67 = 7;
constexpr uint16_t kMtnAes67GetSendPacketTimes = 1;
constexpr uint16_t kMtnAes67GetReceivePacketTimes = 2;
constexpr uint16_t kMtnAes67GetMinReceiveBufferCapacity = 3;
constexpr uint16_t kMtnAes67GetMaxReceiveBufferCapacity = 4;
constexpr uint16_t kMtnAes67GetTransmissionTimeVariation = 5;
constexpr uint16_t kMtnAes67GetSupportedDiscoverySystems = 6;
// Fitcan 私有
constexpr uint16_t kMtnAes67DeleteAllConnectors = 1000;
constexpr uint16_t kMtnAes67UpdateRouteTableCommand = 0x8000;

// ── OcaMediaTransportNetworkAES67 媒体协议编号 ──
constexpr uint8_t kMediaProtocolAes67 = 3;  // AES67

// ProtocolVersion (AES70-2023)
constexpr uint16_t kProtocolVersion = 1;
constexpr uint8_t kSyncVal = 0x3B;

}  // namespace oca::methods

#endif  // OCA_METHODS_HPP_
