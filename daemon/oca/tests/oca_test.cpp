#define BOOST_TEST_MODULE oca_test
#include <boost/test/unit_test.hpp>

#include "oca/methods.hpp"
#include "oca/object.hpp"
#include "oca/ocp1.hpp"
#include "oca/classes/device_manager.hpp"
#include "oca/classes/network_manager.hpp"
#include "oca/classes/root.hpp"
#include "oca/classes/subscription_manager.hpp"
#include "oca/session.hpp"
#include "oca/types.hpp"

namespace {
class StubObject : public oca::Object {
 public:
  explicit StubObject(oca::ONo ono) : oca::Object(ono) {}
  const oca::ClassIdentification& class_id() const override { return id_; }
  uint16_t class_version() const override { return 1; }
  oca::Status exec(oca::MethodID,
                   oca::ocp1::Reader&,
                   oca::ocp1::Writer&,
                   oca::Session&) override {
    return oca::Status::OK;
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
      nullptr, 0);
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
  BOOST_CHECK_EQUAL(cmds[0].paramCount, 0);
}

BOOST_AUTO_TEST_CASE(ocp1_response_and_notification2_roundtrip) {
  // response: handle=7, status=OK, params={0x00,0x01} (u16 OcaVersion=1)
  uint8_t params[2] = {0x00, 0x01};
  oca::ocp1::Writer rw;
  oca::ocp1::write_response(rw, 7, oca::Status::OK, params, 2);
  auto pdu = oca::ocp1::PduWriter::build_response_pdu(1, rw.data(), rw.size());
  auto hdr =
      oca::ocp1::PduReader::try_parse_header(pdu.data() + 1, pdu.size() - 1);
  auto rsps = oca::ocp1::PduReader::parse_responses(
      pdu.data() + 1 + 9, rw.size(), hdr->messageCount);
  BOOST_REQUIRE_EQUAL(rsps.size(), 1u);
  BOOST_CHECK_EQUAL(rsps[0].handle, 7u);
  BOOST_CHECK(rsps[0].statusCode == oca::Status::OK);
  BOOST_CHECK_EQUAL(rsps[0].paramCount, 2);
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
  BOOST_CHECK(st == oca::Status::OK);
  // ClassID = u16 count(3) + 1,1,3 + u16 version(2)
  oca::ocp1::Reader r(rsp.data(), rsp.size());
  BOOST_CHECK_EQUAL(r.u16(), 3u);  // 3 levels
  BOOST_CHECK_EQUAL(r.u16(), 1u);
  BOOST_CHECK_EQUAL(r.u16(), 1u);
  BOOST_CHECK_EQUAL(r.u16(), 3u);
  BOOST_CHECK_EQUAL(r.u16(), 2u);  // classVersion

  // GetMembers {3,5} -> [1,2,4]
  oca::ocp1::Writer rsp2;
  st = root.exec({oca::methods::kDefLevelBlock, oca::methods::kBlockGetMembers},
                 empty, rsp2, sess);
  BOOST_CHECK(st == oca::Status::OK);
  oca::ocp1::Reader r2(rsp2.data(), rsp2.size());
  BOOST_CHECK_EQUAL(r2.u16(), 3u);  // 3 members
  BOOST_CHECK_EQUAL(r2.u32(), 1u);
  BOOST_CHECK_EQUAL(r2.u32(), 2u);
  BOOST_CHECK_EQUAL(r2.u32(), 4u);

  // 未知方法 -> BadMethod
  oca::ocp1::Writer rsp3;
  st = root.exec({oca::methods::kDefLevelBlock, 99}, empty, rsp3, sess);
  BOOST_CHECK(st == oca::Status::BadMethod);

  // 未知 defLevel -> BadMethod
  oca::ocp1::Writer rsp4;
  st = root.exec({99, 1}, empty, rsp4, sess);
  BOOST_CHECK(st == oca::Status::BadMethod);
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
  BOOST_CHECK(st1 == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b1.data(), b1.size()).u16(), 1u);

  // GetDeviceName -> "Studio A"
  auto [st2, b2] = call(oca::methods::kDevGetDeviceName);
  BOOST_CHECK(st2 == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b2.data(), b2.size()).string(),
                    "Studio A");

  // GetModelDescription -> {Acme, AES67-daemon, bondagit-3.1.0}
  auto [st3, b3] = call(oca::methods::kDevGetModelDescription);
  BOOST_CHECK(st3 == oca::Status::OK);
  {
    oca::ocp1::Reader r(b3.data(), b3.size());
    BOOST_CHECK_EQUAL(r.string(), "Acme");
    BOOST_CHECK_EQUAL(r.string(), "AES67-daemon");
    BOOST_CHECK_EQUAL(r.string(), "bondagit-3.1.0");
  }

  // GetState -> Operational(2)
  auto [st4, b4] = call(oca::methods::kDevGetState);
  BOOST_CHECK(st4 == oca::Status::OK);
  BOOST_CHECK_EQUAL(oca::ocp1::Reader(b4.data(), b4.size()).u8(),
                    static_cast<uint8_t>(oca::DeviceState::Operational));

  // GetManagers -> 3 descriptors, first is ONo 1 / Role "DeviceManager"
  auto [st5, b5] = call(oca::methods::kDevGetManagers);
  BOOST_CHECK(st5 == oca::Status::OK);
  {
    oca::ocp1::Reader r(b5.data(), b5.size());
    BOOST_CHECK_EQUAL(r.u16(), 3u);
    BOOST_CHECK_EQUAL(r.u32(), 1u);                  // first manager ONo
    BOOST_CHECK_EQUAL(r.string(), "DeviceManager");  // Name = Role
    uint16_t levels = r.u16();                       // ClassID level count
    BOOST_CHECK_EQUAL(levels, 3u);                   // {1,2,1}
    r.u16();
    r.u16();
    r.u16();                         // skip 1,2,1
    BOOST_CHECK_EQUAL(r.u16(), 4u);  // classVersion
  }

  // 未实现方法 -> BadMethod
  auto [st6, b6] = call(oca::methods::kDevSetDeviceName);
  BOOST_CHECK(st6 == oca::Status::BadMethod);
}

BOOST_AUTO_TEST_CASE(dispatch_network_manager) {
  oca::OcaNetworkManager nm(2);
  oca::Session sess(1);
  oca::ocp1::Reader empty(nullptr, 0);
  oca::ocp1::Writer rsp;
  auto st = nm.exec(
      {oca::methods::kDefLevelNetworkMngr, oca::methods::kNetGetNetworks},
      empty, rsp, sess);
  BOOST_CHECK(st == oca::Status::OK);
  oca::ocp1::Reader r(rsp.data(), rsp.size());
  BOOST_CHECK_EQUAL(r.u16(), 0u);  // 空网络列表

  // GetClassIdentification(继承自 Root) -> {1,2,3} v3
  oca::ocp1::Writer rsp2;
  st = nm.exec(
      {oca::methods::kDefLevelRoot, oca::methods::kRootGetClassIdentification},
      empty, rsp2, sess);
  BOOST_CHECK(st == oca::Status::OK);
  oca::ocp1::Reader r2(rsp2.data(), rsp2.size());
  BOOST_CHECK_EQUAL(r2.u16(), 3u);
  BOOST_CHECK_EQUAL(r2.u16(), 1u);
  BOOST_CHECK_EQUAL(r2.u16(), 2u);
  BOOST_CHECK_EQUAL(r2.u16(), 3u);
  BOOST_CHECK_EQUAL(r2.u16(), 3u);  // classVersion
}

BOOST_AUTO_TEST_CASE(dispatch_subscription_ev2) {
  oca::OcaSubscriptionManager sm(4);
  oca::Session sess(1);
  oca::ObjectRegistry reg;
  sess.set_registry(&reg);

  // 构造 AddSubscription2 请求:emitter=1, event={3,1}, ctx=空
  oca::ocp1::Writer reqw;
  reqw.u32(1);  // EmitterONo
  reqw.u16(oca::methods::kDefLevelDeviceMngr);
  reqw.u16(oca::methods::kEventOperationalState);
  reqw.u16(0);  // 空 subscriberContext
  oca::ocp1::Reader req(reqw.data(), reqw.size());

  oca::ocp1::Writer rspw;
  auto st = sm.exec(
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubAddSubscription2}, req,
      rspw, sess);
  BOOST_CHECK(st == oca::Status::OK);
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

  // RemoveSubscription2
  oca::ocp1::Writer reqw2;
  reqw2.u32(subId);
  oca::ocp1::Reader req2(reqw2.data(), reqw2.size());
  oca::ocp1::Writer rspw2;
  st = sm.exec(
      {oca::methods::kDefLevelSubMngr, oca::methods::kSubRemoveSubscription2},
      req2, rspw2, sess);
  BOOST_CHECK(st == oca::Status::OK);
  BOOST_CHECK(
      !sess.has_subscription(1, {oca::methods::kDefLevelDeviceMngr,
                                 oca::methods::kEventOperationalState}));
}
