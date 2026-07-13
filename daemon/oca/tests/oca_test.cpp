#define BOOST_TEST_MODULE oca_test
#include <boost/test/unit_test.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "oca/methods.hpp"
#include "oca/mdns_publisher.hpp"
#include "oca/oca_server.hpp"
#include "oca/object.hpp"
#include "oca/ocp1.hpp"
#include "oca/classes/agent.hpp"
#include "oca/classes/application_network.hpp"
#include "oca/classes/control_network.hpp"
#include "oca/classes/device_manager.hpp"
#include "oca/classes/network.hpp"
#include "oca/classes/network_manager.hpp"
#include "oca/classes/root.hpp"
#include "oca/classes/subscription_manager.hpp"
#include "oca/session.hpp"
#include "oca/transport.hpp"
#include "oca/types.hpp"

namespace {
class StubObject : public oca::Object {
 public:
  explicit StubObject(oca::ONo ono) : oca::Object(ono) {}
  const oca::ClassIdentification& class_id() const override { return id_; }
  uint16_t class_version() const override { return 1; }
  oca::ExecResult exec(oca::MethodID,
                       oca::ocp1::Reader&,
                       oca::ocp1::Writer&,
                       oca::Session&) override {
    return {oca::Status::OK, 0};
  }

 private:
  oca::ClassIdentification id_{};
};
}  // namespace

BOOST_AUTO_TEST_CASE(types_and_constants) {
  namespace m = oca::methods;
  BOOST_CHECK_EQUAL((int)oca::Status::OK, 0);
  BOOST_CHECK_EQUAL((int)oca::Status::BadONo, 5);
  BOOST_CHECK_EQUAL((int)oca::Status::NotImplemented, 8);
  BOOST_CHECK_EQUAL((int)oca::Status::BadMethod, 11);
  BOOST_CHECK_EQUAL(m::kSyncVal, 0x3B);
  BOOST_CHECK_EQUAL(m::kProtocolVersion, 1);
  BOOST_CHECK_EQUAL(m::kPduKeepAlive, 4);
  BOOST_CHECK_EQUAL(m::kPduNtf2, 5);
  // 关键方法索引(ocac 核对)
  BOOST_CHECK_EQUAL(m::kDevGetOcaVersion, 1);
  BOOST_CHECK_EQUAL(m::kDevGetSerialNumber, 3);
  BOOST_CHECK_EQUAL(m::kDevGetModelDescription, 6);
  BOOST_CHECK_EQUAL(m::kDevGetState, 13);
  BOOST_CHECK_EQUAL(m::kDevGetOperationalState, 23);
  BOOST_CHECK_EQUAL(m::kDevGetManagers, 19);
  BOOST_CHECK_EQUAL(m::kBlockGetMembers, 5);
  BOOST_CHECK_EQUAL(m::kRootGetClassIdentification, 1);
  BOOST_CHECK_EQUAL(m::kRootGetRole, 5);
  // OcaBitstring 默认
  oca::OcaBitstring bs;
  BOOST_CHECK_EQUAL(bs.numBits, 0);
}

BOOST_AUTO_TEST_CASE(ocp1_scalar_roundtrip) {
  oca::ocp1::Writer w;
  w.u8(0x12);
  w.u16(0x1234);
  w.u32(0x12345678);
  w.u64(0x123456789ABCDEF0ULL);
  w.i8(-1);
  w.i16(-1000);
  w.i32(-123456);
  w.i64(-9999999999LL);
  w.f32(3.14f);
  w.f64(2.718281828);

  oca::ocp1::Reader r(w.data(), w.size());
  BOOST_CHECK_EQUAL(r.u8(), 0x12);
  BOOST_CHECK_EQUAL(r.u16(), 0x1234);
  BOOST_CHECK_EQUAL(r.u32(), 0x12345678u);
  BOOST_CHECK_EQUAL(r.u64(), 0x123456789ABCDEF0ULL);
  BOOST_CHECK_EQUAL(r.i8(), -1);
  BOOST_CHECK_EQUAL(r.i16(), -1000);
  BOOST_CHECK_EQUAL(r.i32(), -123456);
  BOOST_CHECK_EQUAL(r.i64(), -9999999999LL);
  BOOST_CHECK_CLOSE(r.f32(), 3.14f, 0.0001);
  BOOST_CHECK_CLOSE(r.f64(), 2.718281828, 0.0000001);
  BOOST_CHECK_EQUAL(r.remaining(), 0u);

  // big-endian 字节序校验
  BOOST_CHECK_EQUAL(w.data()[1], 0x12);  // u16 高字节
  BOOST_CHECK_EQUAL(w.data()[2], 0x34);
}

BOOST_AUTO_TEST_CASE(ocp1_reader_bounds) {
  uint8_t buf[2] = {0x01, 0x02};
  oca::ocp1::Reader r(buf, 2);
  r.u16();                                        // ok
  BOOST_CHECK_THROW(r.u8(), std::runtime_error);  // 越界
}

BOOST_AUTO_TEST_CASE(ocp1_string_codepoints) {
  oca::ocp1::Writer w;
  w.string("AES67");  // 5 码点
  // 码点计数应为 5(=0x0005),后跟 5 字节 ASCII
  BOOST_CHECK_EQUAL(w.size(), 2u + 5u);
  BOOST_CHECK_EQUAL(w.data()[0], 0x00);
  BOOST_CHECK_EQUAL(w.data()[1], 0x05);

  oca::ocp1::Reader r(w.data(), w.size());
  BOOST_CHECK_EQUAL(r.string(), "AES67");

  // 中文/emoji 码点计数(非字节数)
  oca::ocp1::Writer w2;
  w2.string("音频");                      // 2 码点,6 字节 UTF-8
  BOOST_CHECK_EQUAL(w2.size(), 2u + 6u);  // u16 计数 + 字节
  BOOST_CHECK_EQUAL(w2.data()[1], 0x02);  // 码点数=2
  oca::ocp1::Reader r2(w2.data(), w2.size());
  BOOST_CHECK_EQUAL(r2.string(), "音频");

  oca::ocp1::Writer w3;
  w3.string("😀");                        // 1 码点,4 字节 UTF-8
  BOOST_CHECK_EQUAL(w3.data()[1], 0x01);  // 码点数=1
  oca::ocp1::Reader r3(w3.data(), w3.size());
  BOOST_CHECK_EQUAL(r3.string(), "😀");
}

BOOST_AUTO_TEST_CASE(ocp1_blob_and_bitstring) {
  oca::ocp1::Writer w;
  std::vector<uint8_t> b{0xDE, 0xAD, 0xBE, 0xEF};
  w.blob(b);
  // u16 count(4) + 4 bytes
  BOOST_CHECK_EQUAL(w.size(), 6u);
  oca::ocp1::Reader r(w.data(), w.size());
  BOOST_CHECK(r.blob() == b);

  oca::OcaBitstring bs;
  bs.numBits = 10;
  bs.bytes = {0xFF, 0x03};
  oca::ocp1::Writer wb;
  wb.bitstring(bs);
  // u16 numBits(10) + 2 bytes (AES70-3: 无独立 nbytes
  // 字段,字节数=ceil(numBits/8))
  BOOST_CHECK_EQUAL(wb.size(), 4u);
  oca::ocp1::Reader rb(wb.data(), wb.size());
  auto bs2 = rb.bitstring();
  BOOST_CHECK_EQUAL(bs2.numBits, 10u);
  BOOST_CHECK((bs2.bytes == std::vector<uint8_t>{0xFF, 0x03}));
}

BOOST_AUTO_TEST_CASE(ocp1_list_roundtrip) {
  std::vector<uint32_t> in{1, 2, 4, 100};
  oca::ocp1::Writer w;
  w.list<uint32_t>(in,
                   [](oca::ocp1::Writer& ww, const uint32_t& v) { ww.u32(v); });
  oca::ocp1::Reader r(w.data(), w.size());
  auto out = r.list<uint32_t>([](oca::ocp1::Reader& rr) { return rr.u32(); });
  BOOST_CHECK(out == in);
}

BOOST_AUTO_TEST_CASE(ocp1_command_pdu_roundtrip) {
  // 构造一条 command:handle=42, target=1, method {3,1} (GetOcaVersion), 无参
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(
      cw, 42, 1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetOcaVersion},
      nullptr, 0, 0);
  auto pdu = oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size());

  // 校验 sync + header
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->protocolVersion, 1);
  BOOST_CHECK_EQUAL(hdr->pduType, oca::methods::kPduCommand);
  BOOST_CHECK_EQUAL(hdr->messageCount, 1);
  BOOST_CHECK_EQUAL(hdr->pduSize, 9u + cw.size());

  // 解析 command
  const uint8_t* payload = pdu.data() + 1 + 9;
  size_t payloadLen = cw.size();
  auto cmds = oca::ocp1::PduReader::parse_commands(payload, payloadLen,
                                                   hdr->messageCount);
  BOOST_REQUIRE_EQUAL(cmds.size(), 1u);
  BOOST_CHECK_EQUAL(cmds[0].handle, 42u);
  BOOST_CHECK_EQUAL(cmds[0].targetONo, 1u);
  BOOST_CHECK_EQUAL(cmds[0].methodID.defLevel,
                    oca::methods::kDefLevelDeviceMngr);
  BOOST_CHECK_EQUAL(cmds[0].methodID.methodIndex,
                    oca::methods::kDevGetOcaVersion);
  BOOST_CHECK_EQUAL(cmds[0].nrParameters, 0);
  BOOST_CHECK_EQUAL(cmds[0].paramBytes, 0u);
}

BOOST_AUTO_TEST_CASE(ocp1_response_and_notification2_roundtrip) {
  // response: handle=7, status=OK, params={0x00,0x01} (u16 OcaVersion=1)
  uint8_t params[2] = {0x00, 0x01};
  oca::ocp1::Writer rw;
  oca::ocp1::write_response(rw, 7, oca::Status::OK, params, 2, 1);
  auto pdu = oca::ocp1::PduWriter::build_response_pdu(1, rw.data(), rw.size());
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  auto rsps = oca::ocp1::PduReader::parse_responses(
      pdu.data() + 1 + 9, rw.size(), hdr->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK_EQUAL(rsps[0].handle, 7u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rsps[0].nrParameters, 1);
  BOOST_CHECK_EQUAL(rsps[0].paramBytes, 2u);
  BOOST_CHECK_EQUAL(rsps[0].paramData[1], 0x01);

  // notification2: emitter=1, event {3,1}, type=0(event),
  // data={0x00}(DeviceState Operational)
  uint8_t evdata[1] = {static_cast<uint8_t>(oca::DeviceState::Operational)};
  oca::ocp1::Writer nw;
  oca::ocp1::write_notification2(
      nw, 1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kEventOperationalState},
      0, evdata, 1);
  auto npdu =
      oca::ocp1::PduWriter::build_notification2_pdu(1, nw.data(), nw.size());
  auto nhdr =
      oca::ocp1::PduReader::try_parse_header(npdu.data() + 1, npdu.size() - 1);
  BOOST_CHECK_EQUAL(nhdr->pduType, oca::methods::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      npdu.data() + 1 + 9, nw.size(), nhdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex,
                    oca::methods::kEventOperationalState);
  BOOST_CHECK_EQUAL(ntfs[0].dataCount, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].data[0],
                    static_cast<uint8_t>(oca::DeviceState::Operational));
}

BOOST_AUTO_TEST_CASE(ocp1_keepalive_pdu) {
  auto pdu = oca::ocp1::PduWriter::build_keepalive_pdu(5);
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, oca::methods::kPduKeepAlive);
  BOOST_CHECK_EQUAL(hdr->messageCount, 1);
  BOOST_CHECK_EQUAL(hdr->pduSize, 11u);  // 9 header + 2 heartbeat
  // heartbeat = u16 at offset 1(sync)+9(header) = 10
  BOOST_CHECK_EQUAL(pdu[10], 0x00);
  BOOST_CHECK_EQUAL(pdu[11], 0x05);
}

BOOST_AUTO_TEST_CASE(ocp1_fuzz_no_crash) {
  // 随机字节不应让 header 解析崩溃
  uint8_t junk[9] = {0x3B, 0, 1, 0, 0, 0, 9, 0, 0};
  auto hdr = oca::ocp1::PduReader::try_parse_header(junk + 1, 8);
  BOOST_CHECK(!hdr);  // 不足 9 字节
  hdr = oca::ocp1::PduReader::try_parse_header(junk + 1, 8);
  BOOST_CHECK(!hdr);
}

BOOST_AUTO_TEST_CASE(registry_find_and_range) {
  oca::ObjectRegistry reg;
  reg.register_object(std::make_unique<StubObject>(1));
  reg.register_object(std::make_unique<StubObject>(4));
  reg.register_object(std::make_unique<StubObject>(100));

  BOOST_CHECK(reg.find(1) != nullptr);
  BOOST_CHECK(reg.find(4) != nullptr);
  BOOST_CHECK(reg.find(2) == nullptr);  // 未注册
  BOOST_CHECK_EQUAL(reg.size(), 3u);

  auto mgrs = reg.objects_in_range(1, 99);
  BOOST_REQUIRE_EQUAL(mgrs.size(), 2u);  // 1 和 4
  BOOST_CHECK_EQUAL(mgrs[0]->ono(), 1u);
  BOOST_CHECK_EQUAL(mgrs[1]->ono(), 4u);

  auto all = reg.objects_in_range(1, 100);
  BOOST_CHECK_EQUAL(all.size(), 3u);
}

BOOST_AUTO_TEST_CASE(session_subscription_and_queue) {
  oca::Session s(1);
  BOOST_CHECK_EQUAL(s.session_id(), 1u);

  oca::Subscription2 sub;
  sub.emitterONo = 1;
  sub.eventID = {oca::methods::kDefLevelDeviceMngr,
                 oca::methods::kEventOperationalState};
  s.add_subscription(sub);
  s.add_subscription(sub);  // 去重
  BOOST_CHECK_EQUAL(s.subscriptions().size(), 1u);
  BOOST_CHECK(s.has_subscription(1, sub.eventID));

  s.remove_subscription(1, sub.eventID);
  BOOST_CHECK(!s.has_subscription(1, sub.eventID));
  BOOST_CHECK_EQUAL(s.subscriptions().size(), 0u);

  // 写队列
  s.enqueue_notification({0x3B, 0x00, 0x01});
  s.enqueue_notification({0x3B, 0x00, 0x02});
  std::vector<uint8_t> out;
  BOOST_CHECK(s.take_notification(out));
  BOOST_CHECK_EQUAL(out[2], 0x01);
  BOOST_CHECK(s.take_notification(out));
  BOOST_CHECK_EQUAL(out[2], 0x02);
  BOOST_CHECK(!s.take_notification(out));
}

BOOST_AUTO_TEST_CASE(session_keepalive_expiry) {
  oca::Session s(2);
  s.set_heartbeat(5);
  s.touch(100);
  BOOST_CHECK(!s.expired(110));  // 10s < 15s
  BOOST_CHECK(!s.expired(115));  // 15s == 3*5,不超(严格大于)
  BOOST_CHECK(s.expired(116));   // 16s > 15s
}

BOOST_AUTO_TEST_CASE(dispatch_root_block) {
  oca::OcaBlock root(100);
  oca::ObjectRegistry reg;
  reg.register_object(std::make_unique<StubObject>(1));
  reg.register_object(std::make_unique<StubObject>(2));
  reg.register_object(std::make_unique<StubObject>(4));
  reg.register_object(std::make_unique<StubObject>(100));  // root 自己

  oca::Session sess(1);
  sess.set_registry(&reg);

  // GetClassIdentification {1,1} defLevel=1 methodIndex=1
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer rsp;
  auto st = root.exec(
      {oca::methods::kDefLevelRoot, oca::methods::kRootGetClassIdentification},
      empty, rsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  // ClassID = u16 count(3) + 1,1,3 + u16 version(2)
  oca::ocp1::Reader r(rsp.data(), rsp.size());
  BOOST_CHECK_EQUAL(r.u16(), 3u);  // 3 levels
  BOOST_CHECK_EQUAL(r.u16(), 1u);
  BOOST_CHECK_EQUAL(r.u16(), 1u);
  BOOST_CHECK_EQUAL(r.u16(), 3u);
  BOOST_CHECK_EQUAL(r.u16(), 2u);  // classVersion

  // GetMembers {3,5} -> List<ObjectIdentification> [1,2,4]
  // 每个元素 = ONo(u32) + ClassID(fieldCount u16 + levels) + ClassVersion(u16)
  // StubObject 的 class_id 为空(levels=0, version=0)
  oca::ocp1::Writer rsp2;
  st = root.exec({oca::methods::kDefLevelBlock, oca::methods::kBlockGetMembers},
                 empty, rsp2, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  oca::ocp1::Reader r2(rsp2.data(), rsp2.size());
  BOOST_CHECK_EQUAL(r2.u16(), 3u);  // 3 members
  for (int i = 0; i < 3; ++i) {
    BOOST_CHECK_EQUAL(r2.u32(), static_cast<uint32_t>(i == 0   ? 1
                                                      : i == 1 ? 2
                                                               : 4));
    BOOST_CHECK_EQUAL(r2.u16(), 0u);  // ClassID fieldCount = 0 (StubObject)
    BOOST_CHECK_EQUAL(r2.u16(), 1u);  // ClassVersion = 1 (StubObject)
  }

  // GetMembersRecursive {3,6} -> List<OcaBlockMember>;C=100, root 不在内
  // 每个元素 = ONo + ClassID(fieldCount+levels) + ClassVersion +
  // ContainerONo(u32)
  oca::ocp1::Writer rsp_rec;
  st = root.exec(
      {oca::methods::kDefLevelBlock, oca::methods::kBlockGetMembersRecursive},
      empty, rsp_rec, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  oca::ocp1::Reader rrec(rsp_rec.data(), rsp_rec.size());
  BOOST_CHECK_EQUAL(rrec.u16(), 3u);  // 3 members
  for (int i = 0; i < 3; ++i) {
    BOOST_CHECK_EQUAL(rrec.u32(), static_cast<uint32_t>(i == 0   ? 1
                                                        : i == 1 ? 2
                                                                 : 4));
    BOOST_CHECK_EQUAL(rrec.u16(), 0u);    // ClassID fieldCount = 0 (StubObject)
    BOOST_CHECK_EQUAL(rrec.u16(), 1u);    // ClassVersion = 1 (StubObject)
    BOOST_CHECK_EQUAL(rrec.u32(), 100u);  // ContainerONo = root(100)
  }

  // OcaWorker(DefLevel 2)强制方法:根块是 OcaWorker 子类,test4 对其强制
  // GetEnabled/SetEnabled/GetPorts(Spec3)。委托链
  // OcaBlock->OcaWorker->OcaRoot。
  oca::ocp1::Writer rspW1;
  st = root.exec(
      {oca::methods::kDefLevelManager, oca::methods::kWorkerGetEnabled}, empty,
      rspW1, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(rspW1.data(), rspW1.size()).u8(), 1u);

  // SetEnabled 空体探测安全(返 0 params,不越界)
  oca::ocp1::Writer rspW2;
  st = root.exec(
      {oca::methods::kDefLevelManager, oca::methods::kWorkerSetEnabled}, empty,
      rspW2, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  oca::ocp1::Writer rspW3;
  st =
      root.exec({oca::methods::kDefLevelManager, oca::methods::kWorkerGetPorts},
                empty, rspW3, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(rspW3.data(), rspW3.size()).u16(), 0u);

  // 未知方法 -> NotImplemented
  oca::ocp1::Writer rsp3;
  st = root.exec({oca::methods::kDefLevelBlock, 99}, empty, rsp3, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // 未知 defLevel -> BadMethod
  oca::ocp1::Writer rsp4;
  st = root.exec({99, 1}, empty, rsp4, sess);
  BOOST_CHECK(st.status == oca::Status::BadMethod);
}

// Spec3 CM3 网络对象:OcaNetwork{1,2,1} / OcaControlNetwork{1,4,1}
// 最小强制实例。 验证 classID 编码、2018 强制方法返值、OcaRoot
// 基类方法委托(GetClassIdentification)。
BOOST_AUTO_TEST_CASE(dispatch_cm3_network_objects) {
  oca::OcaNetwork net(4097);
  oca::OcaControlNetwork ctrl(4098);
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);

  // OcaNetwork GetClassIdentification(defLevel 1 委托)-> {1,2,1} v1
  oca::ocp1::Writer r1;
  auto st = net.exec(
      {oca::methods::kDefLevelRoot, oca::methods::kRootGetClassIdentification},
      empty, r1, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  oca::ocp1::Reader rd1(r1.data(), r1.size());
  BOOST_CHECK_EQUAL(rd1.u16(), 3u);  // fieldCount
  BOOST_CHECK_EQUAL(rd1.u16(), 1u);
  BOOST_CHECK_EQUAL(rd1.u16(), 2u);
  BOOST_CHECK_EQUAL(rd1.u16(), 1u);
  BOOST_CHECK_EQUAL(rd1.u16(), 1u);  // ClassVersion=1

  // OcaNetwork 强制方法(defLevel 3 == fieldCount)
  oca::ocp1::Writer wLink;
  st = net.exec({oca::methods::kDefLevelBlock, oca::methods::kNet2GetLinkType},
                empty, wLink, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wLink.data(), wLink.size()).u8(), 1u);

  oca::ocp1::Writer wCtrl;
  st = net.exec(
      {oca::methods::kDefLevelBlock, oca::methods::kNet2GetControlProtocol},
      empty, wCtrl, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wCtrl.data(), wCtrl.size()).u8(), 1u);

  oca::ocp1::Writer wMedia;
  st = net.exec(
      {oca::methods::kDefLevelBlock, oca::methods::kNet2GetMediaProtocol},
      empty, wMedia, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wMedia.data(), wMedia.size()).u8(), 0u);

  // OcaNetwork GetIDAdvertised -> OcaBlob:u16 len=0(空 NetworkNodeID)
  oca::ocp1::Writer wId;
  st = net.exec(
      {oca::methods::kDefLevelBlock, oca::methods::kNet2GetIDAdvertised}, empty,
      wId, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wId.data(), wId.size()).u16(), 0u);

  // OcaNetwork GetSystemInterfaces -> 空 List(u16 count=0)
  oca::ocp1::Writer wIf;
  st = net.exec(
      {oca::methods::kDefLevelBlock, oca::methods::kNet2GetSystemInterfaces},
      empty, wIf, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wIf.data(), wIf.size()).u16(), 0u);

  // OcaNetwork 未知方法 -> NotImplemented
  oca::ocp1::Writer wUnk;
  st = net.exec({oca::methods::kDefLevelBlock, 99}, empty, wUnk, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // OcaNetwork Shutdown(13) 空体探测安全 -> OK 0 params
  // (XML 2018 Mandatory=false 但合规工具仍判 mandatory,实装以合规)
  oca::ocp1::Writer wShut;
  st = net.exec({oca::methods::kDefLevelBlock, oca::methods::kNet2Shutdown},
                empty, wShut, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // OcaControlNetwork GetControlProtocol(1) -> OCP.1=1
  oca::ocp1::Writer wCC;
  st = ctrl.exec(
      {oca::methods::kDefLevelBlock, oca::methods::kCtrlNetGetControlProtocol},
      empty, wCC, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wCC.data(), wCC.size()).u8(), 1u);

  // OcaControlNetwork{1,4,1} 前缀匹配 OcaApplicationNetwork{1,4},工具对 4098 也
  // 测 GetServiceID(4)/GetSystemInterfaces(6)(Mandatory=true)。
  // AppNet{1,4} defLevel=fieldCount=2,工具按 defLevel=2 调用。
  oca::ocp1::Writer wSvc;
  st = ctrl.exec(
      {oca::methods::kDefLevelManager, oca::methods::kAppNetGetServiceID},
      empty, wSvc, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wSvc.data(), wSvc.size()).u16(), 0u);

  oca::ocp1::Writer wIf2;
  st = ctrl.exec({oca::methods::kDefLevelManager,
                  oca::methods::kAppNetGetSystemInterfaces},
                 empty, wIf2, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wIf2.data(), wIf2.size()).u16(), 0u);

  // OcaControlNetwork GetClassIdentification -> {1,4,1} v1
  oca::ocp1::Writer r2;
  st = ctrl.exec(
      {oca::methods::kDefLevelRoot, oca::methods::kRootGetClassIdentification},
      empty, r2, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  oca::ocp1::Reader rd2(r2.data(), r2.size());
  BOOST_CHECK_EQUAL(rd2.u16(), 3u);
  BOOST_CHECK_EQUAL(rd2.u16(), 1u);
  BOOST_CHECK_EQUAL(rd2.u16(), 4u);
  BOOST_CHECK_EQUAL(rd2.u16(), 1u);
  BOOST_CHECK_EQUAL(rd2.u16(), 1u);
}

BOOST_AUTO_TEST_CASE(dispatch_agent_methods) {
  // OcaNetwork(4097) 继承 OcaAgent,测试 Agent defLevel=2 方法分派
  oca::OcaNetwork net(4097, 100);  // owner = Root Block
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  namespace m = oca::methods;

  // GetLabel(1) -> role() = "Network"
  oca::ocp1::Writer wLabel;
  auto st =
      net.exec({m::kDefLevelManager, m::kAgentGetLabel}, empty, wLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wLabel.data(), wLabel.size()).string(),
                    "Network");

  // SetLabel(2) -> no-op OK
  oca::ocp1::Writer wSetLabel;
  st = net.exec({m::kDefLevelManager, m::kAgentSetLabel}, empty, wSetLabel,
                sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // GetOwner(3) -> 100 (Root Block)
  oca::ocp1::Writer wOwner;
  st = net.exec({m::kDefLevelManager, m::kAgentGetOwner}, empty, wOwner, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wOwner.data(), wOwner.size()).u32(),
                    100u);

  // GetPath(4) -> NotImplemented
  oca::ocp1::Writer wPath;
  st = net.exec({m::kDefLevelManager, m::kAgentGetPath}, empty, wPath, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // 未知 Agent 方法 -> NotImplemented
  oca::ocp1::Writer wUnk;
  st = net.exec({m::kDefLevelManager, 99}, empty, wUnk, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // defLevel=1 委托到 OcaRoot 仍正常
  oca::ocp1::Writer wRole;
  st = net.exec({m::kDefLevelRoot, m::kRootGetRole}, empty, wRole, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wRole.data(), wRole.size()).string(),
                    "Network");
}

BOOST_AUTO_TEST_CASE(dispatch_appnet_methods) {
  // OcaControlNetwork(4098) 继承 OcaApplicationNetwork,
  // 测试 AppNet defLevel=2 方法分派
  oca::OcaControlNetwork ctrl(4098, 100);  // owner = Root Block
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  namespace m = oca::methods;

  // GetLabel(1) -> role() = "Control Network"
  oca::ocp1::Writer wLabel;
  auto st =
      ctrl.exec({m::kDefLevelManager, m::kAppNetGetLabel}, empty, wLabel, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wLabel.data(), wLabel.size()).string(),
                    "Control Network");

  // SetLabel(2) -> no-op OK
  oca::ocp1::Writer wSetLabel;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetSetLabel}, empty, wSetLabel,
                 sess);
  BOOST_CHECK(st.status == oca::Status::OK);

  // GetOwner(3) -> 100 (Root Block)
  oca::ocp1::Writer wOwner;
  st =
      ctrl.exec({m::kDefLevelManager, m::kAppNetGetOwner}, empty, wOwner, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wOwner.data(), wOwner.size()).u32(),
                    100u);

  // GetServiceID(4) -> 空 OcaString
  oca::ocp1::Writer wSvc;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetServiceID}, empty, wSvc,
                 sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wSvc.data(), wSvc.size()).u16(), 0u);

  // GetSystemInterfaces(6) -> 空 List
  oca::ocp1::Writer wIf;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetSystemInterfaces}, empty,
                 wIf, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wIf.data(), wIf.size()).u16(), 0u);

  // GetPath(10) -> NotImplemented
  oca::ocp1::Writer wPath;
  st = ctrl.exec({m::kDefLevelManager, m::kAppNetGetPath}, empty, wPath, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);
}

BOOST_AUTO_TEST_CASE(dispatch_worker_label_owner) {
  // OcaBlock(100) 继承 OcaWorker,测试 Worker defLevel=2 新增方法
  oca::OcaBlock block(100);  // root block, owner_ono 默认 0
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  namespace m = oca::methods;

  // GetLabel(8) -> role() = "Root Block"
  oca::ocp1::Writer wLabel;
  auto st = block.exec({m::kDefLevelManager, m::kWorkerGetLabel}, empty, wLabel,
                       sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wLabel.data(), wLabel.size()).string(),
                    "Root Block");

  // SetLabel(9) -> no-op OK
  oca::ocp1::Writer wSetLabel;
  st = block.exec({m::kDefLevelManager, m::kWorkerSetLabel}, empty, wSetLabel,
                  sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // GetOwner(10) -> 0 (根块无 owner)
  oca::ocp1::Writer wOwner;
  st = block.exec({m::kDefLevelManager, m::kWorkerGetOwner}, empty, wOwner,
                  sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wOwner.data(), wOwner.size()).u32(), 0u);

  // GetPath(13) -> NotImplemented
  oca::ocp1::Writer wPath;
  st = block.exec({m::kDefLevelManager, m::kWorkerGetPath}, empty, wPath, sess);
  BOOST_CHECK(st.status == oca::Status::NotImplemented);

  // 已有 Worker 方法不受影响
  oca::ocp1::Writer wEnabled;
  st = block.exec({m::kDefLevelManager, m::kWorkerGetEnabled}, empty, wEnabled,
                  sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(wEnabled.data(), wEnabled.size()).u8(),
                    1u);
}

BOOST_AUTO_TEST_CASE(dispatch_property_changed_label_emit) {
  // Spec4:对象级 PropertyChanged 发射链验证。
  // 构造 OcaNetwork(4097,继承 OcaAgent)+ 真实 SubscriptionManager,
  // 单元内手动注入 emitter(模拟 OcaServer 注入),SetLabel 后断言:
  //   - Notification2 PDU 入队,emitterONo=4097,eventID={1,1}
  //   - data = PropertyID{2,1} + OcaString == "hello"
  //   - GetLabel 回读 == "hello"(label_ 已真存)
  //   - emitter=nullptr 时 silent(无 ntf)
  oca::OcaSubscriptionManager sm(4);
  oca::OcaNetwork net(4097, 100);
  net.set_event_emitter(&sm);  // 手动注入
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);
  namespace m = oca::methods;

  // 1) AddPropertyChangeSubscription2(target=4, emitter=4097, PropertyID{2,1})
  //    sphinx §3.10:EmitterONo + PropertyID{u16,u16} + DeliveryMode + NetAddr
  oca::ocp1::Writer reqw;
  reqw.u32(4097);                 // EmitterONo
  reqw.u16(m::kDefLevelManager);  // PropertyID defLevel (Agent 引入级=2)
  reqw.u16(m::kPropLabel);        // PropertyID propertyIndex (Label=1)
  reqw.u8(1);                     // NotificationDeliveryMode = Normal
  reqw.u16(0);                    // 空 NetworkAddress
  oca::ocp1::Reader req(reqw.data(), reqw.size());
  oca::ocp1::Writer rspw;
  auto st =
      sm.exec({m::kDefLevelSubMngr, m::kSubAddPropertyChangeSubscription2}, req,
              rspw, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK(sess.has_subscription(
      4097, {m::kDefLevelRoot, m::kEventPropertyChanged}));

  // 2) SetLabel(Agent defLevel=2, kAgentSetLabel=2, OcaString="hello")
  //    -> 真存 label_ + emit PropertyChanged
  oca::ocp1::Writer labw;
  labw.string("hello");
  oca::ocp1::Reader labreq(labw.data(), labw.size());
  oca::ocp1::Writer setrsp;
  st = net.exec({m::kDefLevelManager, m::kAgentSetLabel}, labreq, setrsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // 3) 取 Notification2 PDU 并解析
  std::vector<uint8_t> pdu;
  BOOST_REQUIRE(sess.take_notification(pdu));
  BOOST_REQUIRE(pdu.size() > 10);
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, m::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      pdu.data() + 1 + 9, pdu.size() - 1 - 9, hdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 4097u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.defLevel, m::kDefLevelRoot);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex, m::kEventPropertyChanged);
  // data = PropertyID{u16,u16} + OcaString
  oca::ocp1::Reader dr(ntfs[0].data, ntfs[0].dataCount);
  BOOST_CHECK_EQUAL(dr.u16(), m::kDefLevelManager);  // PropertyID defLevel=2
  BOOST_CHECK_EQUAL(dr.u16(), m::kPropLabel);  // PropertyID propertyIndex=1
  BOOST_CHECK_EQUAL(dr.string(), "hello");
  BOOST_CHECK(!sess.take_notification(pdu));  // 队列已空

  // 4) GetLabel 回读 == "hello"(label_ 已真存)
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer getrsp;
  st = net.exec({m::kDefLevelManager, m::kAgentGetLabel}, empty, getrsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(getrsp.data(), getrsp.size()).string(),
                    "hello");

  // 5) emitter=nullptr 时 SetLabel silent(无 ntf)
  net.set_event_emitter(nullptr);
  oca::ocp1::Reader labreq2(labw.data(), labw.size());
  oca::ocp1::Writer setrsp2;
  st = net.exec({m::kDefLevelManager, m::kAgentSetLabel}, labreq2, setrsp2,
                sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK(!sess.take_notification(pdu));  // 无新通知
}

BOOST_AUTO_TEST_CASE(dispatch_property_changed_enabled_emit) {
  // Spec4:DeviceManager SetEnabled 发射链,对称于 label emit 用例。
  // emitter=1(DevMgr ONo),PropertyID{3,1} + OcaBoolean(u8)。
  oca::OcaDeviceIdentity id;
  id.manufacturer = "Acme";
  id.model_name = "AES67-daemon";
  id.model_version = "bondagit-3.1.0";
  oca::OcaDeviceManager dm(1, id);
  oca::OcaSubscriptionManager sm(4);
  dm.set_event_emitter(&sm);
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);
  namespace m = oca::methods;

  // 默认 enabled_=true -> GetEnabled=1
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer genrsp;
  auto st =
      dm.exec({m::kDefLevelDeviceMngr, m::kDevGetEnabled}, empty, genrsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(genrsp.data(), genrsp.size()).u8(), 1u);

  // 订阅 PropertyChanged(emitter=1, PropertyID{3,1})
  oca::ocp1::Writer reqw;
  reqw.u32(1);                       // EmitterONo = DevMgr
  reqw.u16(m::kDefLevelDeviceMngr);  // PropertyID defLevel=3
  reqw.u16(m::kPropEnabled);         // PropertyID propertyIndex=1
  reqw.u8(1);                        // DeliveryMode Normal
  reqw.u16(0);                       // 空 NetworkAddress
  oca::ocp1::Reader req(reqw.data(), reqw.size());
  oca::ocp1::Writer rspw;
  st = sm.exec({m::kDefLevelSubMngr, m::kSubAddPropertyChangeSubscription2},
               req, rspw, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK(
      sess.has_subscription(1, {m::kDefLevelRoot, m::kEventPropertyChanged}));

  // SetEnabled(u8=0)->真存 enabled_=false + emit
  oca::ocp1::Writer setw;
  setw.u8(0);
  oca::ocp1::Reader setreq(setw.data(), setw.size());
  oca::ocp1::Writer setrsp;
  st = dm.exec({m::kDefLevelDeviceMngr, m::kDevSetEnabled}, setreq, setrsp,
               sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);

  // 解析 Notification2:data = PropertyID{3,1} + OcaBoolean u8
  std::vector<uint8_t> pdu;
  BOOST_REQUIRE(sess.take_notification(pdu));
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, m::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      pdu.data() + 1 + 9, pdu.size() - 1 - 9, hdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  oca::ocp1::Reader dr(ntfs[0].data, ntfs[0].dataCount);
  BOOST_CHECK_EQUAL(dr.u16(), m::kDefLevelDeviceMngr);
  BOOST_CHECK_EQUAL(dr.u16(), m::kPropEnabled);
  BOOST_CHECK_EQUAL(dr.u8(), 0u);
  BOOST_CHECK(!sess.take_notification(pdu));

  // GetEnabled 回读 == 0(enabled_ 已真存为 false)
  oca::ocp1::Writer genrsp2;
  st = dm.exec({m::kDefLevelDeviceMngr, m::kDevGetEnabled}, empty, genrsp2,
               sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(genrsp2.data(), genrsp2.size()).u8(), 0u);
}

BOOST_AUTO_TEST_CASE(dispatch_device_manager) {
  oca::OcaDeviceIdentity id;
  id.manufacturer = "Acme";
  id.model_name = "AES67-daemon";
  id.model_version = "bondagit-3.1.0";
  id.serial_number = "node-42";
  id.device_name = "Studio A";

  oca::OcaDeviceManager dm(1, id);
  oca::ObjectRegistry reg;
  reg.register_object(std::make_unique<oca::OcaDeviceManager>(1, id));
  reg.register_object(std::make_unique<StubObject>(2));
  reg.register_object(std::make_unique<StubObject>(4));
  oca::Session sess(1);
  sess.set_registry(&reg);
  oca::ocp1::Reader empty(nullptr, 0);

  auto call = [&](uint16_t idx) {
    oca::ocp1::Writer w;
    auto st = dm.exec({oca::methods::kDefLevelDeviceMngr, idx}, empty, w, sess);
    return std::make_pair(st, w.take());
  };

  // GetOcaVersion -> 1
  auto [st1, b1] = call(oca::methods::kDevGetOcaVersion);
  BOOST_CHECK(st1.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st1.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b1.data(), b1.size()).u16(), 1u);

  // GetDeviceName -> "Studio A"
  auto [st2, b2] = call(oca::methods::kDevGetDeviceName);
  BOOST_CHECK(st2.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b2.data(), b2.size()).string(),
                    "Studio A");

  // GetModelDescription -> {Acme, AES67-daemon, bondagit-3.1.0}
  auto [st3, b3] = call(oca::methods::kDevGetModelDescription);
  BOOST_CHECK(st3.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st3.nrParameters, 1);  // 1 个结构化参数
  {
    oca::ocp1::Reader r(b3.data(), b3.size());
    BOOST_CHECK_EQUAL(r.string(), "Acme");
    BOOST_CHECK_EQUAL(r.string(), "AES67-daemon");
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");
  }

  // GetState -> Operational(2)
  auto [st4, b4] = call(oca::methods::kDevGetState);
  BOOST_CHECK(st4.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b4.data(), b4.size()).u8(),
                    static_cast<uint8_t>(oca::DeviceState::Operational));

  // GetOperationalState -> {Generic=NormalOperation(0), Details=空 blob}
  auto [st4b, b4b] = call(oca::methods::kDevGetOperationalState);
  BOOST_CHECK(st4b.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st4b.nrParameters, 1);
  {
    oca::ocp1::Reader r(b4b.data(), b4b.size());
    BOOST_CHECK_EQUAL(r.u8(), 0u);   // OcaDeviceGenericState::NormalOperation
    BOOST_CHECK_EQUAL(r.u16(), 0u);  // 空 OcaBlob Details
  }

  // GetManagers -> 3 descriptors, first is ONo 1 / Role "DeviceManager"
  auto [st5, b5] = call(oca::methods::kDevGetManagers);
  BOOST_CHECK(st5.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st5.nrParameters, 1);
  {
    oca::ocp1::Reader r(b5.data(), b5.size());
    BOOST_CHECK_EQUAL(r.u16(), 3u);
    BOOST_CHECK_EQUAL(r.u32(), 1u);                  // first manager ONo
    BOOST_CHECK_EQUAL(r.string(), "DeviceManager");  // Name = Role
    uint16_t levels = r.u16();                       // ClassID level count
    BOOST_CHECK_EQUAL(levels, 3u);                   // {1,3,1}
    r.u16();
    r.u16();
    r.u16();                         // skip 1,3,1
    BOOST_CHECK_EQUAL(r.u16(), 2u);  // classVersion
  }

  // 未实现方法 -> NotImplemented
  auto [st6, b6] = call(oca::methods::kDevSetDeviceName);
  BOOST_CHECK(st6.status == oca::Status::NotImplemented);
}

BOOST_AUTO_TEST_CASE(dispatch_device_manager_2018_methods) {
  // DeviceManager 2018 强制方法:GetModelGUID(2)/GetEnabled(11)/
  // SetEnabled(12)/GetDeviceRevisionID(20)。phase-1 返 NotImplemented(8)。
  oca::OcaDeviceIdentity id;
  id.manufacturer = "Acme";
  id.model_name = "AES67-daemon";
  id.model_version = "bondagit-3.1.0";
  oca::OcaDeviceManager dm(1, id);
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);

  auto call = [&](uint16_t idx, oca::ocp1::Reader& req) {
    oca::ocp1::Writer w;
    auto st = dm.exec({oca::methods::kDefLevelDeviceMngr, idx}, req, w, sess);
    return std::make_pair(st, w.take());
  };

  // GetModelGUID(2) -> 8 原始字节全 0(OcaModelGUID BlobFixedLen 无前缀)
  auto [st2, b2] = call(oca::methods::kDevGetModelGUID, empty);
  BOOST_CHECK(st2.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st2.nrParameters, 1);
  BOOST_CHECK_EQUAL(b2.size(), 8u);
  for (uint8_t c : b2)
    BOOST_CHECK_EQUAL(c, 0u);

  // GetEnabled(11) -> u8(1)
  auto [st11, b11] = call(oca::methods::kDevGetEnabled, empty);
  BOOST_CHECK(st11.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st11.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b11.data(), b11.size()).u8(), 1u);

  // SetEnabled(12):发空体(模拟 2018 工具探测 nrParameters=0x64 但 paramBytes=0)
  //    + 发 u8(1) 两种,均 OK + 0 参
  auto [st12a, b12a] = call(oca::methods::kDevSetEnabled, empty);
  BOOST_CHECK(st12a.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st12a.nrParameters, 0);
  oca::ocp1::Writer sw;
  sw.u8(1);
  oca::ocp1::Reader sreq(sw.data(), sw.size());
  auto [st12b, b12b] = call(oca::methods::kDevSetEnabled, sreq);
  BOOST_CHECK(st12b.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st12b.nrParameters, 0);
  (void)b12a;
  (void)b12b;

  // GetDeviceRevisionID(20) -> OcaString == model_version
  auto [st20, b20] = call(oca::methods::kDevGetDeviceRevisionID, empty);
  BOOST_CHECK(st20.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st20.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b20.data(), b20.size()).string(),
                    "bondagit-3.1.0");
}

BOOST_AUTO_TEST_CASE(dispatch_device_manager_2023_methods) {
  // DeviceManager 2023 Mandatory 方法:GetManufacturer(3.21)/GetProduct(3.22)。
  // 2018 工具不测(2023 新增),实现达 2023 Annex B 合规(G3/G4)。
  oca::OcaDeviceIdentity id;
  id.manufacturer = "Acme";
  id.model_name = "AES67-daemon";
  id.model_version = "bondagit-3.1.0";
  oca::OcaDeviceManager dm(1, id);
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);

  // GetManufacturer(21) -> OcaManufacturer:
  //   Name(str) + OrganizationID(3 raw bytes) + Website(str)
  //   + BusinessContact(str) + TechnicalContact(str)
  oca::ocp1::Writer w21;
  auto st21 = dm.exec(
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetManufacturer},
      empty, w21, sess);
  BOOST_CHECK(st21.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st21.nrParameters, 1);
  {
    oca::ocp1::Reader r(w21.data(), w21.size());
    BOOST_CHECK_EQUAL(r.string(), "Acme");  // Name
    BOOST_CHECK_EQUAL(r.u8(), 0u);          // OrganizationID byte 0
    BOOST_CHECK_EQUAL(r.u16(), 0u);         // OrganizationID bytes 1-2
    BOOST_CHECK_EQUAL(r.string(), "");      // Website
    BOOST_CHECK_EQUAL(r.string(), "");      // BusinessContact
    BOOST_CHECK_EQUAL(r.string(), "");      // TechnicalContact
  }

  // GetProduct(22) -> OcaProduct = 6 个 OcaString
  oca::ocp1::Writer w22;
  auto st22 =
      dm.exec({oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetProduct},
              empty, w22, sess);
  BOOST_CHECK(st22.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st22.nrParameters, 1);
  {
    oca::ocp1::Reader r(w22.data(), w22.size());
    BOOST_CHECK_EQUAL(r.string(), "AES67-daemon");    // Name
    BOOST_CHECK_EQUAL(r.string(), "AES67-daemon");    // ModelID
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");  // RevisionLevel
    BOOST_CHECK_EQUAL(r.string(), "Acme");            // BrandName
    BOOST_CHECK_EQUAL(r.string(), "");                // UUID
    BOOST_CHECK_EQUAL(r.string(), "");                // Description
  }
}

BOOST_AUTO_TEST_CASE(dispatch_network_manager) {
  oca::OcaNetworkManager nm(2);
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer rsp;
  auto st = nm.exec(
      {oca::methods::kDefLevelNetworkMngr, oca::methods::kNetGetNetworks},
      empty, rsp, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  oca::ocp1::Reader r(rsp.data(), rsp.size());
  BOOST_CHECK_EQUAL(r.u16(), 0u);  // 空网络列表

  // GetStreamNetworks(2)/GetControlNetworks(3)/GetMediaTransportNetworks(4):
  // 各返空 List<ONo>(u16(0)),{OK,1}(2018 强制方法,CM3 网络对象弃用故空)。
  for (uint16_t mid : {oca::methods::kNetGetStreamNetworks,
                       oca::methods::kNetGetControlNetworks,
                       oca::methods::kNetGetMediaTransportNetworks}) {
    oca::ocp1::Writer rr;
    auto s =
        nm.exec({oca::methods::kDefLevelNetworkMngr, mid}, empty, rr, sess);
    BOOST_CHECK(s.status == oca::Status::OK);
    BOOST_CHECK_EQUAL(s.nrParameters, 1);
    BOOST_CHECK_EQUAL(oca::ocp1::Reader(rr.data(), rr.size()).u16(), 0u);
  }

  // GetClassIdentification(继承自 Root) -> {1,3,6} v2
  oca::ocp1::Writer rsp2;
  st = nm.exec(
      {oca::methods::kDefLevelRoot, oca::methods::kRootGetClassIdentification},
      empty, rsp2, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  oca::ocp1::Reader r2(rsp2.data(), rsp2.size());
  BOOST_CHECK_EQUAL(r2.u16(), 3u);
  BOOST_CHECK_EQUAL(r2.u16(), 1u);
  BOOST_CHECK_EQUAL(r2.u16(), 3u);
  BOOST_CHECK_EQUAL(r2.u16(), 6u);
  BOOST_CHECK_EQUAL(r2.u16(), 2u);  // classVersion
}

BOOST_AUTO_TEST_CASE(dispatch_subscription_ev2) {
  oca::OcaSubscriptionManager sm(4);
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);

  // 构造 AddSubscription2 请求(sphinx 2024 §C.1):
  //   OcaEvent{EmitterONo(u32) + EventID(u16,u16)}
  //   + NotificationDeliveryMode(u8, Normal=1) + NetworkAddress(blob)
  oca::ocp1::Writer reqw;
  reqw.u32(1);  // EmitterONo
  reqw.u16(oca::methods::kDefLevelDeviceMngr);
  reqw.u16(oca::methods::kEventOperationalState);
  reqw.u8(1);   // NotificationDeliveryMode = Normal
  reqw.u16(0);  // 空 NetworkAddress
  oca::ocp1::Reader req(reqw.data(), reqw.size());

  oca::ocp1::Writer rspw;
  auto st = sm.exec(
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubAddSubscription2}, req,
      rspw, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 1);
  oca::ocp1::Reader rspr(rspw.data(), rspw.size());
  uint32_t subId = rspr.u32();
  BOOST_CHECK(subId != 0);
  BOOST_CHECK(sess.has_subscription(1, {oca::methods::kDefLevelDeviceMngr,
                                        oca::methods::kEventOperationalState}));

  // 触发 OperationalState 事件 -> 会话写队列应收到 Notification2 PDU
  uint8_t evdata = static_cast<uint8_t>(oca::DeviceState::Operational);
  sm.trigger_event(
      1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kEventOperationalState},
      &evdata, 1);
  std::vector<uint8_t> pdu;
  BOOST_CHECK(sess.take_notification(pdu));
  // 校验 PDU:sync + header(pduType=Ntf2) + notification2
  BOOST_REQUIRE(pdu.size() > 10);
  BOOST_CHECK_EQUAL(pdu[0], 0x3B);
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  BOOST_REQUIRE(hdr);
  BOOST_CHECK_EQUAL(hdr->pduType, oca::methods::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      pdu.data() + 1 + 9, pdu.size() - 1 - 9, hdr->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].dataCount, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].data[0], evdata);
  BOOST_CHECK(!sess.take_notification(pdu));  // 队列已空

  // RemoveSubscription2(sphinx 2024 §C.1):
  //   OcaEvent + NotificationDeliveryMode + NetworkAddress
  //   语义 = 移除所有匹配 event 的订阅(非按 subscriptionID)
  oca::ocp1::Writer reqw2;
  reqw2.u32(1);  // EmitterONo
  reqw2.u16(oca::methods::kDefLevelDeviceMngr);
  reqw2.u16(oca::methods::kEventOperationalState);
  reqw2.u8(1);   // NotificationDeliveryMode = Normal
  reqw2.u16(0);  // 空 NetworkAddress
  oca::ocp1::Reader req2(reqw2.data(), reqw2.size());
  oca::ocp1::Writer rspw2;
  st = sm.exec(
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubRemoveSubscription2},
      req2, rspw2, sess);
  BOOST_CHECK(st.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st.nrParameters, 0);
  BOOST_CHECK(
      !sess.has_subscription(1, {oca::methods::kDefLevelDeviceMngr,
                                 oca::methods::kEventOperationalState}));

  // 自测盲点回归:未实现的方法索引(7)必须返回非 OK(NotImplemented),
  // 不得误命中已实现方法。用 != OK 而非 == BadMethod,兼容默认分支
  // NotImplemented。
  oca::ocp1::Writer badw;
  badw.u32(1);
  badw.u16(oca::methods::kDefLevelDeviceMngr);
  badw.u16(oca::methods::kEventOperationalState);
  badw.u8(1);
  badw.u16(0);
  oca::ocp1::Reader badreq(badw.data(), badw.size());
  oca::ocp1::Writer badrsp;
  auto badst =
      sm.exec({oca::methods::kDefLevelSubMngr, 7}, badreq, badrsp, sess);
  BOOST_CHECK(badst.status != oca::Status::OK);
}

BOOST_AUTO_TEST_CASE(dispatch_subscription_ev1_ev2_propertychange) {
  oca::OcaSubscriptionManager sm(4);
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);

  // EV1 AddSubscription(mi=1,5 参):
  //   OcaEvent{emitter u32 + EventID u16,u16} + OcaMethod{ono u32 + mid
  //   u16,u16}
  //   + OcaBlob(ctx) + DeliveryMode u8 + NetworkAddress(blob)
  oca::ocp1::Writer w1;
  w1.u32(1);                                     // EmitterONo
  w1.u16(oca::methods::kDefLevelDeviceMngr);     // EventID defLevel
  w1.u16(oca::methods::kEventOperationalState);  // EventID eventIndex
  w1.u32(7);                                     // subscriber ONo(忽略)
  w1.u16(1);                                     // subscriber MethodID defLevel
  w1.u16(1);  // subscriber MethodID methodIndex
  w1.u16(0);  // 空 context blob
  w1.u8(1);   // DeliveryMode Normal
  w1.u16(0);  // 空 NetworkAddress
  oca::ocp1::Reader r1(w1.data(), w1.size());
  oca::ocp1::Writer rsp1;
  auto st1 = sm.exec(
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubAddSubscription}, r1,
      rsp1, sess);
  BOOST_CHECK(st1.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st1.nrParameters, 0);  // EV1:0 参,无 subID
  BOOST_CHECK_EQUAL(rsp1.size(), 0u);      // 响应体为空
  BOOST_CHECK(sess.has_subscription(1, {oca::methods::kDefLevelDeviceMngr,
                                        oca::methods::kEventOperationalState}));

  // EV1 RemoveSubscription(mi=2,2 参):OcaEvent + OcaMethod(忽略)
  oca::ocp1::Writer w2;
  w2.u32(1);
  w2.u16(oca::methods::kDefLevelDeviceMngr);
  w2.u16(oca::methods::kEventOperationalState);
  w2.u32(7);
  w2.u16(1);
  w2.u16(1);
  oca::ocp1::Reader r2(w2.data(), w2.size());
  oca::ocp1::Writer rsp2;
  auto st2 = sm.exec(
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubRemoveSubscription},
      r2, rsp2, sess);
  BOOST_CHECK(st2.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st2.nrParameters, 0);
  BOOST_CHECK(
      !sess.has_subscription(1, {oca::methods::kDefLevelDeviceMngr,
                                 oca::methods::kEventOperationalState}));

  // EV1 AddPropertyChangeSubscription(mi=5,6 参):
  //   EmitterONo + PropertyID{u16,u16} + OcaMethod{u32,u16,u16}
  //   + OcaBlob + DeliveryMode u8 + NetworkAddress(blob)
  oca::ocp1::Writer w5;
  w5.u32(1);  // EmitterONo
  w5.u16(1);  // PropertyID defLevel(忽略)
  w5.u16(1);  // PropertyID propertyIndex(忽略)
  w5.u32(7);  // subscriber ONo(忽略)
  w5.u16(1);  // subscriber MethodID defLevel
  w5.u16(1);  // subscriber MethodID methodIndex
  w5.u16(0);  // context(忽略)
  w5.u8(1);   // DeliveryMode(忽略)
  w5.u16(0);  // NetworkAddress(忽略)
  oca::ocp1::Reader r5(w5.data(), w5.size());
  oca::ocp1::Writer rsp5;
  auto st5 = sm.exec({oca::methods::kDefLevelSubMngr,
                      oca::methods::kSubAddPropertyChangeSubscription},
                     r5, rsp5, sess);
  BOOST_CHECK(st5.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st5.nrParameters, 0);
  // 订阅记录为 PropertyChanged 事件 {defLevel 1, eventIndex 1}
  BOOST_CHECK(sess.has_subscription(
      1, {oca::methods::kDefLevelRoot, oca::methods::kEventPropertyChanged}));
  // 触发 PropertyChanged -> 会话写队列收到 Notification2(验证记录生效)
  uint8_t evdata = 0x42;
  sm.trigger_event(
      1, {oca::methods::kDefLevelRoot, oca::methods::kEventPropertyChanged},
      &evdata, 1);
  std::vector<uint8_t> ntf;
  BOOST_CHECK(sess.take_notification(ntf));
  BOOST_CHECK(!sess.take_notification(ntf));  // 队列已空

  // EV1 RemovePropertyChangeSubscription(mi=6,3 参):
  //   EmitterONo + PropertyID + OcaMethod(忽略)
  oca::ocp1::Writer w6;
  w6.u32(1);
  w6.u16(1);
  w6.u16(1);
  w6.u32(7);
  w6.u16(1);
  w6.u16(1);
  oca::ocp1::Reader r6(w6.data(), w6.size());
  oca::ocp1::Writer rsp6;
  auto st6 = sm.exec({oca::methods::kDefLevelSubMngr,
                      oca::methods::kSubRemovePropertyChangeSubscription},
                     r6, rsp6, sess);
  BOOST_CHECK(st6.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st6.nrParameters, 0);
  BOOST_CHECK(!sess.has_subscription(
      1, {oca::methods::kDefLevelRoot, oca::methods::kEventPropertyChanged}));

  // EV2 AddPropertyChangeSubscription2(mi=10,4 参,sphinx):
  //   EmitterONo + PropertyID{u16,u16} + DeliveryMode u8 + NetworkAddress(blob)
  oca::ocp1::Writer w10;
  w10.u32(1);
  w10.u16(1);
  w10.u16(1);
  w10.u8(1);
  w10.u16(0);
  oca::ocp1::Reader r10(w10.data(), w10.size());
  oca::ocp1::Writer rsp10;
  auto st10 = sm.exec({oca::methods::kDefLevelSubMngr,
                       oca::methods::kSubAddPropertyChangeSubscription2},
                      r10, rsp10, sess);
  BOOST_CHECK(st10.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st10.nrParameters, 0);  // EV2 PropertyChange2:0 参(sphinx)
  BOOST_CHECK(sess.has_subscription(
      1, {oca::methods::kDefLevelRoot, oca::methods::kEventPropertyChanged}));

  // EV2 RemovePropertyChangeSubscription2(mi=11,4 参,sphinx)
  oca::ocp1::Writer w11;
  w11.u32(1);
  w11.u16(1);
  w11.u16(1);
  w11.u8(1);
  w11.u16(0);
  oca::ocp1::Reader r11(w11.data(), w11.size());
  oca::ocp1::Writer rsp11;
  auto st11 = sm.exec({oca::methods::kDefLevelSubMngr,
                       oca::methods::kSubRemovePropertyChangeSubscription2},
                      r11, rsp11, sess);
  BOOST_CHECK(st11.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st11.nrParameters, 0);
  BOOST_CHECK(!sess.has_subscription(
      1, {oca::methods::kDefLevelRoot, oca::methods::kEventPropertyChanged}));

  // 参差断言:EV1 AddSubscription(mi=1)=0 参  vs
  //           EV2 AddSubscription2(mi=8)=1 参(subID),两者并存语义正确
  oca::ocp1::Writer w8;
  w8.u32(1);
  w8.u16(oca::methods::kDefLevelDeviceMngr);
  w8.u16(oca::methods::kEventOperationalState);
  w8.u8(1);
  w8.u16(0);
  oca::ocp1::Reader r8(w8.data(), w8.size());
  oca::ocp1::Writer rsp8;
  auto st8 = sm.exec(
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubAddSubscription2}, r8,
      rsp8, sess);
  BOOST_CHECK(st8.status == oca::Status::OK);
  BOOST_CHECK_EQUAL(st8.nrParameters, 1);  // EV2 AddSubscription2:1 参(subID)
  BOOST_CHECK(oca::ocp1::Reader(rsp8.data(), rsp8.size()).u32() != 0u);
  BOOST_CHECK_EQUAL(st1.nrParameters, 0u);  // EV1 mi=1
  BOOST_CHECK_EQUAL(st8.nrParameters, 1u);  // EV2 mi=8
}

BOOST_AUTO_TEST_CASE(dispatch_root_lock_unlock) {
  // OcaRoot Lock(3)/Unlock(4):daemon 不可锁(GetLockable=0),no-op 返回 {OK,0}。
  // 2018 工具对 DeviceManager(1)/SubscriptionManager(4)/NetworkManager(6)
  // 均测。
  oca::OcaDeviceIdentity id;
  id.model_name = "dm";
  oca::OcaDeviceManager dm(1, id);
  oca::OcaSubscriptionManager sm(4);
  oca::OcaNetworkManager nm(6);
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);

  for (oca::Object* obj :
       {static_cast<oca::Object*>(&dm), static_cast<oca::Object*>(&sm),
        static_cast<oca::Object*>(&nm)}) {
    oca::ocp1::Writer rsp;
    auto st = obj->exec({oca::methods::kDefLevelRoot, oca::methods::kRootLock},
                        empty, rsp, sess);
    BOOST_CHECK(st.status == oca::Status::OK);
    BOOST_CHECK_EQUAL(st.nrParameters, 0);
    BOOST_CHECK_EQUAL(rsp.size(), 0u);  // 无响应参数

    oca::ocp1::Writer rsp2;
    st = obj->exec({oca::methods::kDefLevelRoot, oca::methods::kRootUnlock},
                   empty, rsp2, sess);
    BOOST_CHECK(st.status == oca::Status::OK);
    BOOST_CHECK_EQUAL(st.nrParameters, 0);
    BOOST_CHECK_EQUAL(rsp2.size(), 0u);
  }
}

BOOST_AUTO_TEST_CASE(transport_keepalive_and_command) {
  // 构造对象树
  oca::OcaDeviceIdentity id;
  id.model_name = "daemon";
  id.model_version = "v1";
  id.serial_number = "n1";
  id.device_name = "dev";
  oca::ObjectRegistry reg;
  auto* dm = new oca::OcaDeviceManager(1, id);
  auto* nm = new oca::OcaNetworkManager(6);
  auto* sm = new oca::OcaSubscriptionManager(4);
  auto* root = new oca::OcaBlock(100);
  reg.register_object(std::unique_ptr<oca::Object>(dm));
  reg.register_object(std::unique_ptr<oca::Object>(nm));
  reg.register_object(std::unique_ptr<oca::Object>(sm));
  reg.register_object(std::unique_ptr<oca::Object>(root));

  oca::Transport transport(&reg, sm);
  BOOST_REQUIRE(transport.start(0));  // 自动端口
  uint16_t port = transport.port();

  // 连接
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    BOOST_CHECK_EQUAL(::send(sock, p.data(), p.size(), 0), (ssize_t)p.size());
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync;
    if (::recv(sock, &sync, 1, 0) != 1)
      return false;
    if (sync != 0x3B)
      return false;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
      ssize_t r = ::recv(sock, hdr + got, 9 - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h)
      return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr + 9);
    out.resize(9 + plen);
    got = 0;
    while (got < plen) {
      ssize_t r = ::recv(sock, out.data() + 9 + got, plen - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    out.insert(out.begin(), 0x3B);
    return true;
  };

  // 1) KeepAlive(5s) -> 应收到 KeepAlive 回应
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka;
  BOOST_CHECK(recvPdu(ka));
  auto kah =
      oca::ocp1::PduReader::try_parse_header(ka.data() + 1, ka.size() - 1);
  BOOST_REQUIRE(kah);
  BOOST_CHECK_EQUAL(kah->pduType, oca::methods::kPduKeepAlive);

  // 2) GetOcaVersion 命令(handle=7)
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(
      cw, 7, 1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetOcaVersion},
      nullptr, 0, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
  std::vector<uint8_t> rsp;
  BOOST_CHECK(recvPdu(rsp));
  auto rh =
      oca::ocp1::PduReader::try_parse_header(rsp.data() + 1, rsp.size() - 1);
  BOOST_REQUIRE(rh);
  BOOST_CHECK_EQUAL(rh->pduType, oca::methods::kPduResponse);
  auto rsps = oca::ocp1::PduReader::parse_responses(
      rsp.data() + 1 + 9, rh->pduSize - 9, rh->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK_EQUAL(rsps[0].handle, 7u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rsps[0].nrParameters, 1);  // OcaVersion = 1 个参数
  BOOST_CHECK_EQUAL(rsps[0].paramBytes, 2u);   // 2 字节参数块
  // OcaVersion=1 big-endian
  BOOST_CHECK_EQUAL(rsps[0].paramData[0], 0x00);
  BOOST_CHECK_EQUAL(rsps[0].paramData[1], 0x01);

  ::close(sock);
  transport.stop();
}

BOOST_AUTO_TEST_CASE(oca_server_facade) {
  oca::OcaServerConfig cfg;
  cfg.port = 0;  // 自动
  cfg.node_id = "AES67 daemon abc123";
  cfg.daemon_version = "bondagit-3.1.0";
  cfg.manufacturer = "AES67-Linux-Daemon";

  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  // 复用 Task 12 的 sendPdu/recvPdu 逻辑(此处内联简版)
  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    ::send(sock, p.data(), p.size(), 0);
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync;
    if (::recv(sock, &sync, 1, 0) != 1 || sync != 0x3B)
      return false;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
      ssize_t r = ::recv(sock, hdr + got, 9 - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h)
      return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr + 9);
    out.resize(9 + plen);
    got = 0;
    while (got < plen) {
      ssize_t r = ::recv(sock, out.data() + 9 + got, plen - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    out.insert(out.begin(), 0x3B);
    return true;
  };

  // KeepAlive
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka;
  BOOST_CHECK(recvPdu(ka));

  // GetDeviceName(4) -> node_id("AES67 daemon abc123")
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(
      cw, 1, 1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetDeviceName},
      nullptr, 0, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
  std::vector<uint8_t> rsp;
  BOOST_CHECK(recvPdu(rsp));
  auto rh =
      oca::ocp1::PduReader::try_parse_header(rsp.data() + 1, rsp.size() - 1);
  auto rsps = oca::ocp1::PduReader::parse_responses(
      rsp.data() + 1 + 9, rh->pduSize - 9, rh->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::OK);
  oca::ocp1::Reader pr(rsps[0].paramData, rsps[0].paramBytes);
  BOOST_CHECK_EQUAL(pr.string(), "AES67 daemon abc123");

  // GetMembers(5) on Root Block(ONo 100) -> List<ObjectIdentification>
  //  [1,4,6] 管理器 + [4097,4098] CM3 网络对象(Spec3)
  // 每个元素 = ONo + ClassID(fieldCount+levels) + ClassVersion
  oca::ocp1::Writer cw2;
  oca::ocp1::write_command(
      cw2, 2, 100,
      {oca::methods::kDefLevelBlock, oca::methods::kBlockGetMembers}, nullptr,
      0, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw2.data(), cw2.size()));
  std::vector<uint8_t> rsp2;
  BOOST_CHECK(recvPdu(rsp2));
  auto rh2 =
      oca::ocp1::PduReader::try_parse_header(rsp2.data() + 1, rsp2.size() - 1);
  auto rsps2 = oca::ocp1::PduReader::parse_responses(
      rsp2.data() + 1 + 9, rh2->pduSize - 9, rh2->messageCount);
  BOOST_REQUIRE_EQUAL(rsps2.size(), 1u);
  oca::ocp1::Reader pr2(rsps2[0].paramData, rsps2[0].paramBytes);
  BOOST_CHECK_EQUAL(pr2.u16(), 5u);  // 5 members(3 管理器 + 2 CM3)
  // ONo=1 DeviceManager {1,3,1} v2
  BOOST_CHECK_EQUAL(pr2.u32(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);  // ClassID fieldCount
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 2u);  // ClassVersion
  // ONo=4 SubscriptionManager {1,3,4} v2
  BOOST_CHECK_EQUAL(pr2.u32(), 4u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u16(), 4u);
  BOOST_CHECK_EQUAL(pr2.u16(), 2u);
  // ONo=6 NetworkManager {1,3,6} v2
  BOOST_CHECK_EQUAL(pr2.u32(), 6u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u16(), 6u);
  BOOST_CHECK_EQUAL(pr2.u16(), 2u);
  // ONo=4097 OcaNetwork {1,2,1} v1(Spec3 CM3,DeprecatedSince 2018)
  BOOST_CHECK_EQUAL(pr2.u32(), 4097u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 2u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);  // ClassVersion=1
  // ONo=4098 OcaControlNetwork {1,4,1} v1(Spec3 CM3,AvailableSince 2018)
  BOOST_CHECK_EQUAL(pr2.u32(), 4098u);
  BOOST_CHECK_EQUAL(pr2.u16(), 3u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 4u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);
  BOOST_CHECK_EQUAL(pr2.u16(), 1u);  // ClassVersion=1

  ::close(sock);
  server.stop();
}

#ifdef _USE_AVAHI_
BOOST_AUTO_TEST_CASE(mdns_publisher_smoke) {
  // 需要 avahi-daemon 运行才真正发布;此处仅验证 start/stop 不崩溃
  oca::MdnsPublisher pub("aes67-oca-test", 65037);
  BOOST_CHECK(pub.start());
  pub.stop();
  // 重复 stop 不崩溃
  pub.stop();
}
#endif

BOOST_AUTO_TEST_CASE(oca_e2e_acceptance) {
  oca::OcaServerConfig cfg;
  cfg.port = 0;
  cfg.node_id = "AES67 daemon e2e";
  cfg.daemon_version = "bondagit-3.1.0";
  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    BOOST_REQUIRE_EQUAL(::send(sock, p.data(), p.size(), 0), (ssize_t)p.size());
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync;
    if (::recv(sock, &sync, 1, 0) != 1 || sync != 0x3B)
      return false;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
      ssize_t r = ::recv(sock, hdr + got, 9 - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h)
      return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr + 9);
    out.resize(9 + plen);
    got = 0;
    while (got < plen) {
      ssize_t r = ::recv(sock, out.data() + 9 + got, plen - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    out.insert(out.begin(), 0x3B);
    return true;
  };
  auto cmd = [&](uint32_t handle, oca::ONo target,
                 oca::MethodID mid) -> oca::ocp1::Response {
    oca::ocp1::Writer cw;
    oca::ocp1::write_command(cw, handle, target, mid, nullptr, 0, 0);
    sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
    std::vector<uint8_t> rsp;
    BOOST_REQUIRE(recvPdu(rsp));
    auto h =
        oca::ocp1::PduReader::try_parse_header(rsp.data() + 1, rsp.size() - 1);
    auto rsps = oca::ocp1::PduReader::parse_responses(
        rsp.data() + 1 + 9, h->pduSize - 9, h->messageCount);
    BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
    return rsps[0];
  };

  // 1) KeepAlive
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka;
  BOOST_REQUIRE(recvPdu(ka));

  // 2) 身份:GetOcaVersion=1
  auto r1 =
      cmd(1, 1,
          {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetOcaVersion});
  BOOST_CHECK(r1.statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(r1.nrParameters, 1);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(r1.paramData, r1.paramBytes).u16(), 1u);

  // 3) 身份:GetModelDescription -> {mfr, model=version, version}
  auto r2 = cmd(2, 1,
                {oca::methods::kDefLevelDeviceMngr,
                 oca::methods::kDevGetModelDescription});
  BOOST_CHECK(r2.statusCode == oca::Status::OK);
  {
    oca::ocp1::Reader r(r2.paramData, r2.paramBytes);
    BOOST_CHECK_EQUAL(r.string(), "AES67-Linux-Daemon");
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");
  }

  // 4) 发现:GetMembers(ONo 100) -> List<ObjectIdentification>
  //     [1,4,6] 管理器 + [4097,4098] CM3 网络对象(Spec3)
  auto r3 = cmd(3, 100,
                {oca::methods::kDefLevelBlock, oca::methods::kBlockGetMembers});
  BOOST_CHECK(r3.statusCode == oca::Status::OK);
  {
    oca::ocp1::Reader r(r3.paramData, r3.paramBytes);
    BOOST_CHECK_EQUAL(r.u16(), 5u);  // 5 members(3 管理器 + 2 CM3)
    // 每个元素 = ONo + ClassID(fieldCount+levels) + ClassVersion;只校验 ONo
    BOOST_CHECK_EQUAL(r.u32(), 1u);
    r.u16();  // skip ClassID fieldCount
    r.u16();
    r.u16();
    r.u16();  // skip 3 ClassID levels
    r.u16();  // skip ClassVersion
    BOOST_CHECK_EQUAL(r.u32(), 4u);
    r.u16();
    r.u16();
    r.u16();
    r.u16();
    r.u16();
    BOOST_CHECK_EQUAL(r.u32(), 6u);
    r.u16();
    r.u16();
    r.u16();
    r.u16();
    r.u16();
    // ONo=4097 OcaNetwork {1,2,1} v1(CM3)
    BOOST_CHECK_EQUAL(r.u32(), 4097u);
    r.u16();
    r.u16();
    r.u16();
    r.u16();
    r.u16();
    // ONo=4098 OcaControlNetwork {1,4,1} v1(CM3)
    BOOST_CHECK_EQUAL(r.u32(), 4098u);
    r.u16();
    r.u16();
    r.u16();
    r.u16();
    r.u16();
  }

  // 5) 订阅:AddSubscription2(emitter=1, OperationalState) (sphinx 2024 §C.1)
  //    命令参数:OcaEvent{emitterONo(u32) + eventID(u16,u16)}
  //            + NotificationDeliveryMode(u8, Normal=1) + NetworkAddress(blob)
  //    = 3 个参数
  oca::ocp1::Writer params;
  params.u32(1);  // EmitterONo
  params.u16(oca::methods::kDefLevelDeviceMngr);
  params.u16(oca::methods::kEventOperationalState);
  params.u8(1);   // NotificationDeliveryMode = Normal
  params.u16(0);  // 空 NetworkAddress
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(
      cw, 5, 4,
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubAddSubscription2},
      params.data(), static_cast<uint32_t>(params.size()), 3);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
  std::vector<uint8_t> rspsub;
  BOOST_REQUIRE(recvPdu(rspsub));
  auto hs = oca::ocp1::PduReader::try_parse_header(rspsub.data() + 1,
                                                   rspsub.size() - 1);
  auto subrsps = oca::ocp1::PduReader::parse_responses(
      rspsub.data() + 1 + 9, hs->pduSize - 9, hs->messageCount);
  BOOST_REQUIRE_EQUAL(subrsps.size(), 1u);
  BOOST_CHECK(subrsps[0].statusCode == oca::Status::OK);
  uint32_t subId =
      oca::ocp1::Reader(subrsps[0].paramData, subrsps[0].paramBytes).u32();

  // 6) 触发事件,然后发一个 ping 让传输层排空通知队列
  uint8_t evdata = static_cast<uint8_t>(oca::DeviceState::Operational);
  server.subscription_manager()->trigger_event(
      1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kEventOperationalState},
      &evdata, 1);
  cmd(4, 1,
      {oca::methods::kDefLevelDeviceMngr,
       oca::methods::kDevGetOcaVersion});  // ping -> 触发排空

  // 7) 收 Notification2
  std::vector<uint8_t> ntf;
  BOOST_REQUIRE(recvPdu(ntf));
  auto hn =
      oca::ocp1::PduReader::try_parse_header(ntf.data() + 1, ntf.size() - 1);
  BOOST_CHECK_EQUAL(hn->pduType, oca::methods::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      ntf.data() + 1 + 9, hn->pduSize - 9, hn->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex,
                    oca::methods::kEventOperationalState);
  BOOST_CHECK_EQUAL(ntfs[0].dataCount, 1u);
  BOOST_CHECK_EQUAL(ntfs[0].data[0], evdata);

  (void)subId;
  ::close(sock);
  server.stop();
}

BOOST_AUTO_TEST_CASE(oca_e2e_property_changed) {
  // Spec4 里程碑:全真实 socket 端到端 PropertyChanged 投递。
  // 经真实 SetLabel 命令触发(不伸手 trigger_event),验证:
  //   订阅 -> SetLabel -> transport drain -> Notification2 上线 -> 解析
  // 与 oca_e2e_acceptance 区别:后者 OperationalState 经测试代码直接触发,
  // 本用例 PropertyChanged 经真实 setter 触发,证明发射点已接入生产路径。
  oca::OcaServerConfig cfg;
  cfg.port = 0;
  cfg.node_id = "AES67 daemon pc";
  cfg.daemon_version = "bondagit-3.1.0";
  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    BOOST_REQUIRE_EQUAL(::send(sock, p.data(), p.size(), 0), (ssize_t)p.size());
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync;
    if (::recv(sock, &sync, 1, 0) != 1 || sync != 0x3B)
      return false;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
      ssize_t r = ::recv(sock, hdr + got, 9 - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h)
      return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr + 9);
    out.resize(9 + plen);
    got = 0;
    while (got < plen) {
      ssize_t r = ::recv(sock, out.data() + 9 + got, plen - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    out.insert(out.begin(), 0x3B);
    return true;
  };
  namespace m = oca::methods;
  // 接收穿插的 Notification2 必须缓存而非丢弃:transport 在发送 Response 后
  // 排空通知队列,与同一命令的 Response 在单流上交错;cmd 等待 Response 时
  // 可能先收到 Ntf2。将这类穿插通知暂存在待审队列,供步骤 5 取用。
  std::vector<std::vector<uint8_t>> pending_ntfs;
  // 带参命令封装:SetLabel/订阅 需传参。接收时循环跳过穿插的通知,只取
  // pduType=Response 的 PDU;途中遇到的 Ntf2 存入 pending_ntfs。
  auto cmdParams = [&](uint32_t handle, oca::ONo target, oca::MethodID mid,
                       const uint8_t* params, uint32_t paramBytes,
                       uint8_t nrParams) -> oca::ocp1::Response {
    oca::ocp1::Writer cw;
    oca::ocp1::write_command(cw, handle, target, mid, params, paramBytes,
                             nrParams);
    sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));
    for (int i = 0; i < 16; ++i) {
      std::vector<uint8_t> rsp;
      BOOST_REQUIRE(recvPdu(rsp));
      auto h = oca::ocp1::PduReader::try_parse_header(rsp.data() + 1,
                                                      rsp.size() - 1);
      if (h->pduType == m::kPduResponse) {
        auto rsps = oca::ocp1::PduReader::parse_responses(
            rsp.data() + 1 + 9, h->pduSize - 9, h->messageCount);
        BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
        return rsps[0];
      }
      if (h->pduType == m::kPduNtf2)
        pending_ntfs.push_back(std::move(rsp));  // 缓存穿插通知
      // 其他 PduType(KeepAlive)忽略
    }
    BOOST_FAIL("超时:未收到 Response PDU");
    return {};
  };
  auto cmd = [&](uint32_t handle, oca::ONo target,
                 oca::MethodID mid) -> oca::ocp1::Response {
    return cmdParams(handle, target, mid, nullptr, 0, 0);
  };
  // 取下一个 Notification2 PDU:先消费缓存的穿插通知,再 recv。
  auto recvNtf2 = [&]() -> std::vector<uint8_t> {
    for (int i = 0; i < 32; ++i) {
      if (!pending_ntfs.empty()) {
        auto p = std::move(pending_ntfs.front());
        pending_ntfs.erase(pending_ntfs.begin());
        return p;
      }
      std::vector<uint8_t> rsp;
      BOOST_REQUIRE(recvPdu(rsp));
      auto h = oca::ocp1::PduReader::try_parse_header(rsp.data() + 1,
                                                      rsp.size() - 1);
      if (h->pduType == m::kPduNtf2)
        return rsp;
      // 其他 PduType 忽略,继续等
    }
    BOOST_FAIL("超时:未收到 Notification2 PDU");
    return {};
  };

  // 1) KeepAlive 握手
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka;
  BOOST_REQUIRE(recvPdu(ka));

  // 2) AddPropertyChangeSubscription2(target=4, emitter=4097, PropertyID{2,1})
  oca::ocp1::Writer subp;
  subp.u32(4097);                 // EmitterONo
  subp.u16(m::kDefLevelManager);  // PropertyID defLevel=2(Agent)
  subp.u16(m::kPropLabel);        // PropertyID propertyIndex=1
  subp.u8(1);                     // DeliveryMode Normal
  subp.u16(0);                    // 空 NetworkAddress
  auto rsub = cmdParams(
      1, 4, {m::kDefLevelSubMngr, m::kSubAddPropertyChangeSubscription2},
      subp.data(), static_cast<uint32_t>(subp.size()), 4);
  BOOST_CHECK(rsub.statusCode == oca::Status::OK);

  // 3) SetLabel(4097, kAgentSetLabel=2, OcaString="world")
  oca::ocp1::Writer labw;
  labw.string("world");
  auto rset = cmdParams(2, 4097, {m::kDefLevelManager, m::kAgentSetLabel},
                        labw.data(), static_cast<uint32_t>(labw.size()), 1);
  BOOST_CHECK(rset.statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rset.nrParameters, 0);

  // 4) ping(GetOcaVersion)->强制 transport 排空通知队列,通知上线
  auto rping = cmd(3, 1, {m::kDefLevelDeviceMngr, m::kDevGetOcaVersion});
  BOOST_CHECK(rping.statusCode == oca::Status::OK);

  // 5) 取 Notification2(消费缓存或 recv;穿插通知经 cmd 已存 pending_ntfs)
  std::vector<uint8_t> ntf = recvNtf2();
  auto hn =
      oca::ocp1::PduReader::try_parse_header(ntf.data() + 1, ntf.size() - 1);
  BOOST_CHECK_EQUAL(hn->pduType, m::kPduNtf2);
  auto ntfs = oca::ocp1::PduReader::parse_notifications2(
      ntf.data() + 1 + 9, hn->pduSize - 9, hn->messageCount);
  BOOST_REQUIRE_EQUAL(ntfs.size(), 1u);
  BOOST_CHECK_EQUAL(ntfs[0].emitterONo, 4097u);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.defLevel, m::kDefLevelRoot);
  BOOST_CHECK_EQUAL(ntfs[0].eventID.eventIndex, m::kEventPropertyChanged);
  oca::ocp1::Reader dr(ntfs[0].data, ntfs[0].dataCount);
  BOOST_CHECK_EQUAL(dr.u16(), m::kDefLevelManager);
  BOOST_CHECK_EQUAL(dr.u16(), m::kPropLabel);
  BOOST_CHECK_EQUAL(dr.string(), "world");

  // 6) 回读 GetLabel(4097) -> "world"(label_ 经 OcaServer 已真存)
  auto rget = cmd(4, 4097, {m::kDefLevelManager, m::kAgentGetLabel});
  BOOST_CHECK(rget.statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(rget.paramData, rget.paramBytes).string(),
                    "world");

  ::close(sock);
  server.stop();
}

BOOST_AUTO_TEST_CASE(transport_empty_body_probe_keeps_connection) {
  // 回归(阶段二真实控制器抓包坐实):2018 工具在 OCC Object Compliancy test5
  // 方法存在性探测阶段,对 SubscriptionManager 发空体 AddSubscription(nrParams
  // 字段填 0x64 但 paramBytes=0)。EV1 AddSubscription 第一行 req.u32() 越界抛
  // 异常;transport 曾整个 PDU 一个 try/catch,单命令异常即 break 断连,致后续
  // 所有命令 "Failed to send result=2"、AddSubscription 报 Timeout(13)。
  // 修复:per-command try/catch,单命令异常返 BadFormat,连接存活。
  oca::OcaServerConfig cfg;
  cfg.port = 0;
  cfg.node_id = "AES67 daemon probe";
  cfg.daemon_version = "v1";
  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  auto sendPdu = [&](const std::vector<uint8_t>& p) {
    BOOST_REQUIRE_EQUAL(::send(sock, p.data(), p.size(), 0), (ssize_t)p.size());
  };
  auto recvPdu = [&](std::vector<uint8_t>& out) -> bool {
    uint8_t sync;
    if (::recv(sock, &sync, 1, 0) != 1 || sync != 0x3B)
      return false;
    uint8_t hdr[9];
    size_t got = 0;
    while (got < 9) {
      ssize_t r = ::recv(sock, hdr + got, 9 - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    auto h = oca::ocp1::PduReader::try_parse_header(hdr, 9);
    if (!h)
      return false;
    size_t plen = h->pduSize - 9;
    out.assign(hdr, hdr + 9);
    out.resize(9 + plen);
    got = 0;
    while (got < plen) {
      ssize_t r = ::recv(sock, out.data() + 9 + got, plen - got, 0);
      if (r <= 0)
        return false;
      got += r;
    }
    out.insert(out.begin(), 0x3B);
    return true;
  };

  // KeepAlive 握手
  sendPdu(oca::ocp1::PduWriter::build_keepalive_pdu(5));
  std::vector<uint8_t> ka;
  BOOST_REQUIRE(recvPdu(ka));

  // 空体 AddSubscription(target=4, {3,1}, 无参数块) - 模拟工具探测
  oca::ocp1::Writer cw;
  oca::ocp1::write_command(
      cw, 1, 4,
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubAddSubscription},
      nullptr, 0, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw.data(), cw.size()));

  // 关键断言 1:必须收到响应(连接未断),status=BadFormat(4)(非 Timeout/断连)
  std::vector<uint8_t> rsp;
  BOOST_REQUIRE(recvPdu(rsp));
  auto rh =
      oca::ocp1::PduReader::try_parse_header(rsp.data() + 1, rsp.size() - 1);
  auto rsps = oca::ocp1::PduReader::parse_responses(
      rsp.data() + 1 + 9, rh->pduSize - 9, rh->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::BadFormat);

  // 关键断言 2:连接存活 - 后续 GetOcaVersion 正常返回 OK
  oca::ocp1::Writer cw2;
  oca::ocp1::write_command(
      cw2, 2, 1,
      {oca::methods::kDefLevelDeviceMngr, oca::methods::kDevGetOcaVersion},
      nullptr, 0, 0);
  sendPdu(oca::ocp1::PduWriter::build_command_pdu(1, cw2.data(), cw2.size()));
  std::vector<uint8_t> rsp2;
  BOOST_REQUIRE(recvPdu(rsp2));
  auto rh2 =
      oca::ocp1::PduReader::try_parse_header(rsp2.data() + 1, rsp2.size() - 1);
  auto rsps2 = oca::ocp1::PduReader::parse_responses(
      rsp2.data() + 1 + 9, rh2->pduSize - 9, rh2->messageCount);
  BOOST_REQUIRE_EQUAL(rsps2.size(), 1u);
  BOOST_CHECK(rsps2[0].statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rsps2[0].paramBytes, 2u);

  ::close(sock);
  server.stop();
}

BOOST_AUTO_TEST_CASE(transport_rejects_oversized_pdu) {
  // 回归:畸形 pduSize(0xFFFFFFFF)不得触发超大 payload 分配致 daemon 崩溃。
  // 服务端应在分配前因 pduSize 越界关闭该连接,测试进程存活。
  oca::OcaServerConfig cfg;
  cfg.port = 0;
  cfg.node_id = "AES67 daemon ovs";
  cfg.daemon_version = "v1";
  oca::OcaServer server(cfg);
  BOOST_REQUIRE(server.start());
  uint16_t port = server.port();

  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  BOOST_REQUIRE(
      ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  // 畸形 PDU:sync(3B)+header(protoVer=1, pduSize=0xFFFFFFFF, type=Command,
  // msgCount=1),不附 payload。未修时 payloadLen≈4GB 触发 bad_alloc 逃逸线程
  // -> std::terminate -> SIGABRT,本测试进程崩溃,无法到达下方断言。
  std::vector<uint8_t> bad{0x3B, 0x00, 0x01, 0xFF, 0xFF,
                           0xFF, 0xFF, 0x00, 0x00, 0x01};
  BOOST_REQUIRE_EQUAL(::send(sock, bad.data(), bad.size(), 0),
                      (ssize_t)bad.size());

  // 服务端应关闭连接(recv 返回 0);进程不崩溃。
  uint8_t dummy;
  ssize_t r = ::recv(sock, &dummy, 1, 0);
  BOOST_CHECK(r <= 0);

  ::close(sock);
  server.stop();
}
