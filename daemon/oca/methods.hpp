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

// ProtocolVersion (AES70-2023)
constexpr uint16_t kProtocolVersion = 1;
constexpr uint8_t kSyncVal = 0x3B;

}  // namespace oca::methods

#endif  // OCA_METHODS_HPP_
