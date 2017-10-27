// Copyright 2017 Qihoo
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http:// www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "src/meta/zp_meta_server.h"

#include <sys/resource.h>
#include <glog/logging.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/repeated_field.h>

#include <string>
#include <utility>
#include <algorithm>

#include "pink/include/pink_cli.h"
#include "slash/include/env.h"
#include "slash/include/slash_coding.h"
#include "src/meta/zp_meta.pb.h"
#include "src/meta/zp_meta_update_thread.h"
#include "src/meta/zp_meta_condition_cron.h"
#include "src/meta/zp_meta_election.h"
#include "src/meta/zp_meta_migrate_register.h"

ZPMetaServer::ZPMetaServer()
  : should_exit_(false),
  server_thread_(NULL),
  role_(MetaRole::kNone) {
  LOG(INFO) << "ZPMetaServer start initialization";

  // Init Command
  cmds_.reserve(300);
  InitClientCmdTable();

  // Open Floyd
  Status s = OpenFloyd();
  if (!s.ok()) {
    LOG(FATAL) << "Failed to open floyd, error: " << s.ToString();
  }

  // Init Election
  election_ = new ZPMetaElection(floyd_);

  // Open InfoStore
  info_store_ = new ZPMetaInfoStore(floyd_);

  // Init Migrate Register
  migrate_register_ = new ZPMetaMigrateRegister(floyd_);

  // Init update thread
  update_thread_ = new ZPMetaUpdateThread(info_store_,
      migrate_register_);

  // Init Condition thread
  condition_cron_ = new ZPMetaConditionCron(info_store_,
      migrate_register_, update_thread_);

  // Init Server thread
  conn_factory_ = new ZPMetaClientConnFactory();
  server_thread_ = pink::NewDispatchThread(
      g_zp_conf->local_port() + kMetaPortShiftCmd,
      g_zp_conf->meta_thread_num(),
      conn_factory_,
      kMetaDispathCronInterval,
      kMetaDispathQueueSize,
      nullptr);
  server_thread_->set_thread_name("ZPMetaDispatch");
  server_thread_->set_keepalive_timeout(kKeepAlive);
}

ZPMetaServer::~ZPMetaServer() {
  if (server_thread_ != NULL) {
    server_thread_->StopThread();
  }
  delete server_thread_;
  delete conn_factory_;

  delete condition_cron_;
  delete update_thread_;
  delete migrate_register_;
  delete info_store_;
  delete election_;
  delete floyd_;

  slash::MutexLock l(&(leader_joint_.mutex));
  leader_joint_.CleanLeader();
  DestoryCmdTable(cmds_);
  LOG(INFO) << "ZPMetaServer Delete Done";
}

Status ZPMetaServer::OpenFloyd() {
  int port = 0;
  std::string ip;
  floyd::Options fy_options;
  fy_options.members = g_zp_conf->meta_addr();
  for (std::string& it : fy_options.members) {
    std::replace(it.begin(), it.end(), '/', ':');
    if (!slash::ParseIpPortString(it, ip, port)) {
      LOG(WARNING) << "Error meta addr: " << it;
      return Status::Corruption("Error meta addr");
    }
    it = slash::IpPortString(ip, port + kMetaPortShiftFY);
  }
  fy_options.local_ip = g_zp_conf->local_ip();
  fy_options.local_port = g_zp_conf->local_port() + kMetaPortShiftFY;
  fy_options.path = g_zp_conf->data_path();
  fy_options.check_leader_us = g_zp_conf->floyd_check_leader_us();
  fy_options.heartbeat_us = g_zp_conf->floyd_heartbeat_us();
  return floyd::Floyd::Open(fy_options, &floyd_);
}

void ZPMetaServer::Start() {
  LOG(INFO) << "ZPMetaServer started on port:" << g_zp_conf->local_port();

  if (0 != server_thread_->StartThread()) {
    LOG(FATAL) << "Disptch thread start failed";
    return;
  }
  LOG(INFO) << "Start server thread succ: " << std::hex
    << server_thread_->thread_id(); 

  while (!should_exit_) {
    DoTimingTask();
    int sleep_count = kMetaCronWaitCount;
    while (!should_exit_ && sleep_count-- > 0) {
      usleep(kMetaCronInterval * 1000);
    }
  }
  return;
}

Cmd* ZPMetaServer::GetCmd(const int op) {
  return GetCmdFromTable(op, cmds_);
}

Status ZPMetaServer::GetMetaInfoByTable(const std::string& table,
    ZPMeta::MetaCmdResponse_Pull *ms_info) {
  // Get epoch first and because the epoch was updated at last.
  //
  // A newer table meta info with older epoch is acceptable,
  // which leaves the retry operation to node server
  ms_info->set_version(info_store_->epoch());
  ZPMeta::Table* table_info = ms_info->add_info();
  Status s = info_store_->GetTableMeta(table, table_info);
  if (!s.ok()) {
    LOG(WARNING) << "Get table meta for node failed: " << s.ToString()
      << ", table: " << table;
    return s;
  }
  return Status::OK();
}

Status ZPMetaServer::GetMetaInfoByNode(const std::string& ip_port,
    ZPMeta::MetaCmdResponse_Pull *ms_info) {
  // Get epoch first and because the epoch was updated at last.
  //
  // A newer table meta info with older epoch is acceptable,
  // which leaves the retry operation to node server
  ms_info->set_version(info_store_->epoch());

  std::set<std::string> tables;
  Status s = info_store_->GetTablesForNode(ip_port, &tables);
  if (!s.ok() && !s.IsNotFound()) {
    LOG(WARNING) << "Get all tables for Node failed: " << s.ToString()
      << ", node: " << ip_port;
    return s;
  }

  for (const auto& t : tables) {
    ZPMeta::Table* table_info = ms_info->add_info();
    s = info_store_->GetTableMeta(t, table_info);
    if (!s.ok()) {
      LOG(WARNING) << "Get one table meta for node failed: " << s.ToString()
        << ", node: " << ip_port << ", table: " << t;
      return s;
    }
  }
  return Status::OK();
}

bool ZPMetaServer::TableExist(const std::string& table) {
  std::set<std::string> table_list;
  Status s = info_store_->GetTableList(&table_list);
  if (!s.ok()) {
    LOG(WARNING) << "Get table list failed: " << s.ToString();
    return false;
  }
  if (table_list.find(table) != table_list.end()) {
    return true;
  }
  return false;
}

Status ZPMetaServer::CreateTable(const ZPMeta::Table& table) {
  const std::string& table_name = table.name();
  if (TableExist(table_name)) {
    return Status::InvalidArgument("Table already exist");
  }

  // Update command such like
  // Init, DropTable, SetMaster, AddSlave and RemoveSlave
  // were handled asynchronously
  UpdateTask task;
  task.op = kOpAddTable;
  task.opt2 = table_name;
  table.SerializeToString(&task.opt3);
  Status s = update_thread_->PendingUpdate(task);
  if (!s.ok()) {
    LOG(WARNING) << "Pending CreateTable task failed: " << s.ToString()
      << ", table: " << table_name;
    return s;
  }
  return Status::OK();
}

Status ZPMetaServer::DropTable(const std::string& table) {
  // Check
  if (!TableExist(table)) {
    return Status::InvalidArgument("Table not exist");
  }

  // Update command such like
  // Init, DropTable, SetMaster, AddSlave and RemoveSlave
  // were handled asynchronously
  UpdateTask task;
  task.op = kOpRemoveTable;
  task.opt2 = table;
  Status s = update_thread_->PendingUpdate(task);
  if (!s.ok()) {
    LOG(WARNING) << "Pending RemoveTable task failed: " << s.ToString()
      << "Table: " << table;
    return s;
  }
  return Status::OK();
}

Status ZPMetaServer::AddPartitionSlave(const std::string& table, int pnum,
    const ZPMeta::Node& target) {
  // Check node is already slave
  if (info_store_->IsSlave(table, pnum, target)
      || info_store_->IsMaster(table, pnum, target)) {
    return Status::InvalidArgument("Partition not exist or Already exist");
  }

  UpdateTask task;
  task.op = kOpAddSlave;
  task.opt1 = slash::IpPortString(target.ip(), target.port());
  task.opt2 = table;
  task.opt5 = pnum;
  Status s = update_thread_->PendingUpdate(task);
  if (!s.ok()) {
    LOG(WARNING) << "Pending AddSlave task failed: " << s.ToString()
      << "Table: " << table << ", pnum: " << pnum
      << ", target: " << target.ip() << ":" << target.port();
    return s;
  }

  return Status::OK();
}

Status ZPMetaServer::RemovePartitionSlave(const std::string& table, int pnum,
    const ZPMeta::Node& target) {
  // Check node is already slave
  if (!info_store_->IsSlave(table, pnum, target)) {
    return Status::InvalidArgument("Partition not exist or not slave");
  }

  // Just approximately check, not atomic between check and set
  if (migrate_register_->ExistWithLock()) {
    return Status::Corruption("Migrate exist");
  }

  UpdateTask task;
  task.op = kOpRemoveSlave;
  task.opt1 = slash::IpPortString(target.ip(), target.port());
  task.opt2 = table;
  task.opt5 = pnum;
  Status s = update_thread_->PendingUpdate(task);
  if (!s.ok()) {
    LOG(WARNING) << "Pending RemoveSlave task failed: " << s.ToString()
      << "Table: " << table << ", pnum: " << pnum
      << ", target: " << target.ip() << ":" << target.port();
    return s;
  }

  return Status::OK();
}

Status ZPMetaServer::RemoveNodes(
    const ZPMeta::MetaCmd_RemoveNodes& remove_nodes_cmd) {
  std::unordered_map<std::string, NodeInfo> node_infos;
  info_store_->GetAllNodes(&node_infos);
  for (int i = 0; i < remove_nodes_cmd.nodes_size(); i++) {
    const ZPMeta::Node& node = remove_nodes_cmd.nodes(i);
    std::string n = slash::IpPortString(node.ip(), node.port());
    if (node_infos.find(n) != node_infos.end() &&
        node_infos[n].StateEqual(ZPMeta::NodeState::UP)) {
      LOG(WARNING) << "Someone try remove node which is online";
      return Status::Corruption("Cannot remove online node");
    }
  }

  UpdateTask task;
  task.op = kOpRemoveNodes;
  remove_nodes_cmd.SerializeToString(&task.opt3);
  Status s = update_thread_->PendingUpdate(task);
  if (!s.ok()) {
    LOG(WARNING) << "Pending RemoveNodes task failed: ";
    for (int i = 0; i < remove_nodes_cmd.nodes_size(); i++) {
      const ZPMeta::Node& node = remove_nodes_cmd.nodes(i);
      LOG(WARNING) << "  --- " << node.ip() << ":" << node.port();
    }
    return s;
  }

  return Status::OK();
}

Status ZPMetaServer::WaitSetMaster(const ZPMeta::Node& node,
    const std::string table, int partition) {
  // Check node is slave
  if (!info_store_->IsSlave(table, partition, node)) {
    return Status::InvalidArgument("Invaild slave");
  }

  ZPMeta::Node master;
  Status s = info_store_->GetPartitionMaster(table, partition, &master);
  if (!s.ok()) {
    return s;
  }

  UpdateTask task1, task2, task3;

  // Stuck partition
  task1.op = kOpSetStuck;
  task1.opt2 = table;
  task1.opt5 = partition;
  s = update_thread_->PendingUpdate(task1);
  if (!s.ok()) {
    LOG(WARNING) << "Pending SetMaster task failed: " << s.ToString()
      << "Table: " << table << "_" << partition;
    return s;
  }

  // SetMaster when current node catched up with the master
  task2.op = kOpSetMaster;
  task2.opt1 = slash::IpPortString(node.ip(), node.port());
  task2.opt2 = table;
  task2.opt5 = partition;

  task3.op = kOpSetActive;
  task3.opt2 = table;
  task3.opt5 = partition;

  std::vector<UpdateTask> updates = {
    task2,  // Handover from old node to new
    task3,  // Recover Active
  };

  condition_cron_->AddCronTask(
      OffsetCondition(
        ConditionTaskType::kSetMaster,
        table,
        partition,
        master,
        node),
        updates);

  return Status::OK();
}

void ZPMetaServer::UpdateNodeInfo(const ZPMeta::MetaCmd_Ping &ping) {
  if (!info_store_->UpdateNodeInfo(ping)) {
    UpdateTask task;  // new node
    task.op = kOpUpNode;
    task.opt1 = slash::IpPortString(ping.node().ip(), ping.node().port());
    LOG(INFO) << "Pending Update Node Up task: " << task.opt1 <<
      ", node: " << ping.node().ip() << ":" << ping.node().port();

    Status s = update_thread_->PendingUpdate(task);
    if (!s.ok()) {
      LOG(WARNING) << "Pending Update Node Up failed: " << s.ToString() <<
        ", node: " << ping.node().ip() << ":" << ping.node().port();
    }
  }
}

void ZPMetaServer::CheckNodeAlive() {
  std::set<std::string> nodes;
  info_store_->FetchExpiredNode(&nodes);
  for (const auto& n : nodes) {
    LOG(INFO) << "PendingUpdate to remove Node Alive: " << n;
    UpdateTask task;
    task.op = kOpDownNode;
    task.opt1 = n;
    Status s = update_thread_->PendingUpdate(task);
    if (!s.ok()) {
      LOG(WARNING) << "Pending Update Node Down failed: " << s.ToString() <<
        ", " << n;
    }
  }
}

Status ZPMetaServer::GetAllMetaNodes(ZPMeta::MetaCmdResponse_ListMeta *nodes) {
  std::string value;
  ZPMeta::Nodes allnodes;
  std::vector<std::string> meta_nodes;
  floyd_->GetAllNodes(&meta_nodes);

  ZPMeta::MetaNodes *p = nodes->mutable_nodes();
  std::string leader_ip;
  int leader_port = 0;
  bool ret = election_->GetLeader(&leader_ip, &leader_port);
  if (ret) {
    ZPMeta::Node *np = p->mutable_leader();
    np->set_ip(leader_ip);
    np->set_port(leader_port);
  }

  std::string ip;
  int port = 0;
  for (const auto& iter : meta_nodes) {
    if (!slash::ParseIpPortString(iter, ip, port)) {
      return Status::Corruption("parse ip port error");
    }
    if (ret
        && ip == leader_ip
        && port == leader_port + kMetaPortShiftFY) {
      continue;
    }
    ZPMeta::Node *np = p->add_followers();
    np->set_ip(ip);
    np->set_port(port - kMetaPortShiftFY);
  }
  return Status::OK();
}

Status ZPMetaServer::GetMetaStatus(ZPMeta::MetaCmdResponse_MetaStatus* ms) {
  ms->set_version(info_store_->epoch());
  std::string* floyd_status = ms->mutable_consistency_stautus();
  floyd_->GetServerStatus(floyd_status);
  ZPMeta::MigrateStatus migrate_s;
  Status s = migrate_register_->Check(&migrate_s);
  if (s.ok()) {
    ms->mutable_migrate_status()->CopyFrom(migrate_s);
  }
  return Status::OK();
}

// Check whether node is response for specified partition
bool ZPMetaServer::IsCharged(const std::string& table,
    int pnum, const ZPMeta::Node& target) {
  return info_store_->IsSlave(table, pnum, target)
    || info_store_->IsMaster(table, pnum, target);
}

Status ZPMetaServer::GetTableList(std::set<std::string>* table_list) {
  return info_store_->GetTableList(table_list);
}

Status ZPMetaServer::GetNodeStatusList(
    std::unordered_map<std::string, NodeInfo>* node_infos) {
  info_store_->GetAllNodes(node_infos);
  return Status::OK();
}

Status ZPMetaServer::Migrate(int epoch,
    const std::vector<ZPMeta::RelationCmdUnit>& diffs) {
  if (epoch != info_store_->epoch()) {
    return Status::InvalidArgument("Expired epoch");
  }

  // Register
  assert(!diffs.empty());
  Status s = migrate_register_->Init(diffs);
  if (!s.ok()) {
    LOG(WARNING) << "Migrate register Init failed, error: " << s.ToString();
  }

  // Leave the begin of ProcessMigrate in the cron
  return s;
}

Status ZPMetaServer::CancelMigrate() {
  return migrate_register_->Cancel();
}

void ZPMetaServer::ProcessMigrateIfNeed() {
  // Get next
  std::vector<ZPMeta::RelationCmdUnit> diffs;
  Status s = migrate_register_->GetN(kMetaMigrateOnceCount, &diffs);
  if (!s.ok()) {
    if (!s.IsNotFound()) {
      LOG(WARNING) << "Get next N migrate diffs failed, error: "
        << s.ToString();
    }
    return;
  }
  LOG(INFO) << "Begin Process " << diffs.size() << " migrate item";

  for (const auto& diff : diffs) {
    UpdateTask task1, task2, task3, task4;
    // Add Slave
    task1.op = kOpAddSlave;
    task1.opt1 = slash::IpPortString(diff.right().ip(), diff.right().port());
    task1.opt2 = diff.table();
    task1.opt5 = diff.partition();
    s = update_thread_->PendingUpdate(task1);

    // Stuck partition
    task2.op = kOpSetStuck;
    task2.opt2 = diff.table();
    task2.opt5 = diff.partition();
    if (s.ok()) {
      s = update_thread_->PendingUpdate(task2);
    }

    if (!s.ok()) {
      LOG(WARNING) << "Pending migrate item failed: " << s.ToString();
      break;
    }

    // Begin offset condition wait
    task3.op = kOpHandover;
    task3.opt1 = slash::IpPortString(diff.left().ip(), diff.left().port());
    task3.opt2 = slash::IpPortString(diff.right().ip(), diff.right().port());
    task3.opt3 = diff.table();
    task3.opt5 = diff.partition();

    task4.op = kOpSetActive;
    task4.opt2 = diff.table();
    task4.opt5 = diff.partition();
    std::vector<UpdateTask> updates = {
      task3,  // Handover from old node to new
      task4,  // Recover Active
    };

    condition_cron_->AddCronTask(
        OffsetCondition(
          ConditionTaskType::kMigrate,
          diff.table(),
          diff.partition(),
          diff.left(),
          diff.right()),
          updates);
  }
}

Status ZPMetaServer::RedirectToLeader(const ZPMeta::MetaCmd &request,
    ZPMeta::MetaCmdResponse *response) {
  Status s;
  slash::MutexLock l(&(leader_joint_.mutex));
  
  // Connect if needed
  if (leader_joint_.cli == NULL) {
    if (leader_joint_.NoLeader()) {
      return Status::Incomplete("Leader electing");
    }
    leader_joint_.cli = pink::NewPbCli();
    s = leader_joint_.cli->Connect(leader_joint_.ip,
        leader_joint_.port);
    if (!s.ok()) {
      leader_joint_.Disconnect();
      LOG(ERROR) << "Connect to leader: " << leader_joint_.ip
        << ":" << leader_joint_.port << " failed: " << s.ToString();
      return s;
    }
    LOG(INFO) << "Connect to leader: " << leader_joint_.ip
      << ":" << leader_joint_.port << " success.";
    leader_joint_.cli->set_send_timeout(1000);
    leader_joint_.cli->set_recv_timeout(1000);
  }
  
  // Redirect
  s = leader_joint_.cli->Send(const_cast<ZPMeta::MetaCmd*>(&request));
  if (!s.ok()) {
    leader_joint_.Disconnect();
    LOG(ERROR) << "Failed to send redirect message to leader, error: "
      << s.ToString() << ", leader: " << leader_joint_.ip
      << ":" << leader_joint_.port;
    return s;
  }
  s = leader_joint_.cli->Recv(response);
  if (!s.ok()) {
    leader_joint_.Disconnect();
    LOG(ERROR) << "Failed to recv redirect message from leader, error: "
      << s.ToString() << ", leader: " << leader_joint_.ip
      << ":" << leader_joint_.port;
  }
  return s;
}

Status ZPMetaServer::RefreshLeader() {
  std::string leader_ip;
  int leader_port = 0, leader_cmd_port = 0;
  if (!election_->GetLeader(&leader_ip, &leader_port)) {
    LOG(WARNING) << "No leader yet";
    slash::MutexLock l(&(leader_joint_.mutex));
    if (role_ == MetaRole::kLeader) {
      // Give up leadership when floyd failed
      // to avoid error in network partition
      update_thread_->Abandon();
      condition_cron_->Abandon();
      LOG(WARNING) <<
        "Old leader give up leadership since no floyd leader found";
    }
    leader_joint_.CleanLeader();
    role_ = MetaRole::kNone;
    return Status::Incomplete("No leader yet");
  }

  leader_cmd_port = leader_port + kMetaPortShiftCmd;
  slash::MutexLock l(&(leader_joint_.mutex));
  if (role_ == MetaRole::kLeader
      && (leader_ip == g_zp_conf->local_ip()
        && leader_cmd_port == g_zp_conf->local_port())) {
    // I'm Leader, and stay the same
    return Status::OK();
  } else if (role_ == MetaRole::kFollower
      && (leader_ip == leader_joint_.ip
        && leader_cmd_port == leader_joint_.port)) {
    // I'm Follower, and leader stay the same
    return Status::OK();
  }

  // Leader changed
  LOG(WARNING) << "Leader changed from: "
    << leader_joint_.ip << ":" << leader_joint_.port
    << ", To: " <<  leader_ip << ":" << leader_cmd_port;

  // Clear
  role_ = MetaRole::kNone;
  leader_joint_.CleanLeader();

  // I'm new leader
  Status s;
  if (leader_ip == g_zp_conf->local_ip() &&
      leader_cmd_port == g_zp_conf->local_port()) {
    LOG(INFO) << "Become leader: " << leader_ip << ":" << leader_port;

    // Delay a period of time to wait for the old leader's demotion
    sleep(2 * kMetaCronWaitCount * kMetaCronInterval / 1000);

    // Refresh table info
    s = info_store_->Refresh();
    if (!s.ok()) {
      LOG(WARNING) << "Refresh table info failed: " << s.ToString();
      return s;
    }

    // Restore NodeInfo
    s = info_store_->RestoreNodeInfos();
    if (!s.ok()) {
      LOG(ERROR) << "Restore Node infos failed: " << s.ToString();
      return s;
    }
    LOG(INFO) << "Restore Node infos succ";

    // Load Migrate
    s = migrate_register_->Load();
    if (!s.ok()) {
      LOG(ERROR) << "Load Migrate failed: " << s.ToString();
      return s;
    }
    LOG(INFO) << "Load Migrate succ";
    
    // Active Update
    update_thread_->Active();
    LOG(INFO) << "Update thread active succ";

    // Active Condition
    condition_cron_->Active();
    LOG(INFO) << "Condition thread active succ";
    
    // Kill all conns to trigger all client to reconnect
    // to refresh node infomation
    server_thread_->KillAllConns();

    role_ = MetaRole::kLeader;
    return Status::OK();
  }

  // Abandon CronThread and UpdateThread
  // It's safe to just abandon all tasks of update thread, since:
  //  As our design, most of the task could be retry outside,
  //  such as those were launched by Ping or Migrate Process.
  //  The rest comes from admin command,
  //  whose lost is acceptable and could be retry by administrator.
  condition_cron_->Abandon();
  LOG(INFO) << "Condition thread abandon finish";
  
  update_thread_->Abandon();
  LOG(INFO) << "Update thread abandon finish";

  // Kill all conns to trigger all client to reconnect
  // to refresh node infomation
  server_thread_->KillAllConns();

  // Record new leader
  leader_joint_.ip = leader_ip;
  leader_joint_.port = leader_cmd_port;
  role_ = MetaRole::kFollower;
  return Status::OK();
}

void ZPMetaServer::InitClientCmdTable() {
  // Ping Command
  Cmd* pingptr = new PingCmd(kCmdFlagsRead | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::PING),
        pingptr));

  // Pull Command
  Cmd* pullptr = new PullCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::PULL),
        pullptr));

  // Init Command
  Cmd* initptr = new InitCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::INIT),
        initptr));

  // SetMaster Command
  Cmd* setmasterptr = new SetMasterCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::SETMASTER),
        setmasterptr));

  // AddSlave Command
  Cmd* addslaveptr = new AddSlaveCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::ADDSLAVE),
        addslaveptr));

  // RemoveSlave Command
  Cmd* removeslaveptr = new RemoveSlaveCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::REMOVESLAVE),
        removeslaveptr));

  // ListTable Command
  Cmd* listtableptr = new ListTableCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::LISTTABLE),
        listtableptr));

  // ListNode Command
  Cmd* listnodeptr = new ListNodeCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::LISTNODE),
        listnodeptr));

  // ListMeta Command
  Cmd* listmetaptr = new ListMetaCmd(kCmdFlagsRead);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::LISTMETA),
        listmetaptr));

  // MetaStatus Command
  Cmd* meta_status_ptr = new MetaStatusCmd(kCmdFlagsRead | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::METASTATUS),
        meta_status_ptr));

  // DropTable Command
  Cmd* droptableptr = new DropTableCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::DROPTABLE),
        droptableptr));

  // Migrate Command
  Cmd* migrateptr = new MigrateCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::MIGRATE),
        migrateptr));

  // Cancel Migrate Command
  Cmd* cancel_migrate_ptr = new CancelMigrateCmd(kCmdFlagsWrite
      | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(
          ZPMeta::Type::CANCELMIGRATE), cancel_migrate_ptr));

  Cmd* remove_nodes_ptr = new RemoveNodesCmd(kCmdFlagsWrite | kCmdFlagsRedirect);
  cmds_.insert(std::pair<int, Cmd*>(static_cast<int>(ZPMeta::Type::REMOVENODES),
        remove_nodes_ptr));
}



void ZPMetaServer::ResetLastSecQueryNum() {
  uint64_t cur_time_us = slash::NowMicros();
  statistic.last_qps = (statistic.query_num - statistic.last_query_num)
    * 1000000
    / (cur_time_us - statistic.last_time_us + 1);
  statistic.last_query_num = statistic.query_num.load();
  statistic.last_time_us = cur_time_us;
}

void ZPMetaServer::DoTimingTask() {
  Status s = RefreshLeader();
  if (!s.ok()) {
    LOG(WARNING) << "Refresh Leader failed: " << s.ToString();
  }

  if (role_ == MetaRole::kLeader) {  // Is Leader
    // Check alive
    CheckNodeAlive();

    // Process Migrate if needed
    ProcessMigrateIfNeed();
  } else if (role_ == MetaRole::kFollower) {
    // Refresh table info
    s = info_store_->Refresh();
    if (!s.ok()) {
      LOG(WARNING) << "Refresh table info failed: " << s.ToString();
    }

    // Refresh node info
    s = info_store_->RefreshNodeInfos();
    if (!s.ok()) {
      LOG(WARNING) << "Refresh node info failed: " << s.ToString();
    }
  }

  // Update statistic info
  ResetLastSecQueryNum();
  LOG(INFO) << "ServerQueryNum: " << statistic.query_num
    << " ServerCurrentQps: " << statistic.last_qps
    << " Role: " << MetaRoleMsg[role_];
}
