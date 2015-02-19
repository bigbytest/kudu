// Copyright (c) 2014, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
//
// Ksck, a tool to run a Kudu System Check.

#ifndef KUDU_TOOLS_KSCK_H
#define KUDU_TOOLS_KSCK_H

#include <string>
#include <tr1/memory>
#include <tr1/unordered_map>
#include <utility>
#include <vector>

#include "kudu/common/schema.h"
#include "kudu/util/countdown_latch.h"
#include "kudu/util/locks.h"
#include "kudu/util/status.h"

namespace kudu {
class MonoDelta;
namespace tools {

// Representation of a tablet replica on a tablet server.
class KsckTabletReplica {
 public:
  KsckTabletReplica(const std::string ts_uuid, const bool is_leader, const bool is_follower)
      : is_leader_(is_leader),
        is_follower_(is_follower),
        ts_uuid_(ts_uuid) {
  }

  const bool& is_leader() const {
    return is_leader_;
  }

  const bool& is_follower() const {
    return is_follower_;
  }

  const std::string& ts_uuid() const {
    return ts_uuid_;
  }

 private:
  const bool is_leader_;
  const bool is_follower_;
  const std::string ts_uuid_;
  DISALLOW_COPY_AND_ASSIGN(KsckTabletReplica);
};

// Representation of a tablet belonging to a table. The tablet is composed of replicas.
class KsckTablet {
 public:
  // TODO add start/end keys, stale.
  explicit KsckTablet(const std::string& id)
      : id_(id) {
  }

  const std::string& id() const {
    return id_;
  }

  const std::vector<std::tr1::shared_ptr<KsckTabletReplica> >& replicas() const {
    return replicas_;
  }

  void set_replicas(std::vector<std::tr1::shared_ptr<KsckTabletReplica> >& replicas) {
    replicas_.assign(replicas.begin(), replicas.end());
  }
 private:
  const std::string id_;
  std::vector<std::tr1::shared_ptr<KsckTabletReplica> > replicas_;
  DISALLOW_COPY_AND_ASSIGN(KsckTablet);
};

// Representation of a table. Composed of tablets.
class KsckTable {
 public:
  KsckTable(const std::string& name, const Schema& schema, int num_replicas)
      : name_(name),
        schema_(schema),
        num_replicas_(num_replicas) {
  }

  const std::string& name() const {
    return name_;
  }

  const Schema& schema() const {
    return schema_;
  }

  int num_replicas() const {
    return num_replicas_;
  }

  void set_tablets(std::vector<std::tr1::shared_ptr<KsckTablet> >& tablets) {
    tablets_.assign(tablets.begin(), tablets.end());
  }

  std::vector<std::tr1::shared_ptr<KsckTablet> >& tablets() {
    return tablets_;
  }

 private:
  const std::string name_;
  const Schema schema_;
  const int num_replicas_;
  std::vector<std::tr1::shared_ptr<KsckTablet> > tablets_;
  DISALLOW_COPY_AND_ASSIGN(KsckTable);
};

// Class to act as a collector of scan results.
// Provides thread-safe accessors to update and read a hash table of results.
class ChecksumResultReporter {
 public:
  typedef std::pair<Status, uint64_t> ResultPair;
  typedef std::tr1::unordered_map<std::string, ResultPair> ReplicaResultMap;
  typedef std::tr1::unordered_map<std::string, ReplicaResultMap> TabletResultMap;

  // Initialize reporter with the number of replicas being queried.
  explicit ChecksumResultReporter(int num_tablet_replicas);

  // Write an entry to the result map indicating a response from the remote.
  void ReportResult(const std::string& tablet_id, const std::string& replica_uuid,
                    uint64_t checksum) {
    HandleResponse(tablet_id, replica_uuid, Status::OK(), checksum);
  }

  // Write an entry to the result map indicating a some error from the remote.
  void ReportError(const std::string& tablet_id, const std::string& replica_uuid,
                   const Status& status) {
    HandleResponse(tablet_id, replica_uuid, status, 0);
  }

  // Blocks until either the number of results plus errors reported equals
  // num_tablet_replicas (from the constructor), or until the timeout expires,
  // whichever comes first.
  // Returns false if the timeout expired before all responses came in.
  // Otherwise, returns true.
  bool WaitFor(const MonoDelta& timeout) const { return responses_.WaitFor(timeout); }

  // Returns true iff all replicas have reported in.
  bool AllReported() const { return responses_.count() == 0; }

  // Get reported results.
  TabletResultMap checksums() const;

 private:
  // Report either a success or error response.
  void HandleResponse(const std::string& tablet_id, const std::string& replica_uuid,
                      const Status& status, uint64_t checksum);

  CountDownLatch responses_;
  mutable simple_spinlock lock_; // Protects 'checksums_'.
  // checksums_ is an unordered_map of { tablet_id : { replica_uuid : checksum } }.
  TabletResultMap checksums_;
};

// The following two classes must be extended in order to communicate with their respective
// components. The two main use cases envisioned for this are:
// - To be able to mock a cluster to more easily test the Ksck checks.
// - To be able to communicate with a real Kudu cluster.

// Class that must be extended to represent a tablet server.
class KsckTabletServer {
 public:
  explicit KsckTabletServer(const std::string& uuid)
      : uuid_(uuid) {
  }
  virtual ~KsckTabletServer() { }

  // Connects to the configured Tablet Server.
  virtual Status Connect() = 0;

  // Returns true iff Connect() has been called and was successful.
  virtual bool IsConnected() const = 0;

  // Calls Connect() unless IsConnected() returns true. Helper method.
  virtual Status EnsureConnected() {
    if (IsConnected()) return Status::OK();
    return Connect();
  }

  // Run a checksum scan on the associated hosted tablet.
  // If the returned Status == OK, the handler is guaranteed to eventually
  // call back to one of the reporter's methods.
  // Otherwise, the reporter will not be called (you should do this yourself).
  virtual Status RunTabletChecksumScanAsync(
                  const std::string& tablet_id,
                  const Schema& schema,
                  const std::tr1::shared_ptr<ChecksumResultReporter>& reporter) = 0;

  virtual const std::string& uuid() const {
    return uuid_;
  }

  virtual const std::string& address() const = 0;

 private:
  const std::string uuid_;
  DISALLOW_COPY_AND_ASSIGN(KsckTabletServer);
};

// Class that must be extended to represent a master.
class KsckMaster {
 public:
  // Map of KsckTabletServer objects keyed by tablet server permanent_uuid.
  typedef std::tr1::unordered_map<std::string, std::tr1::shared_ptr<KsckTabletServer> > TSMap;

  KsckMaster() { }
  virtual ~KsckMaster() { }

  // Connects to the configured Master.
  virtual Status Connect() = 0;

  // Returns true iff Connect() has been called and was successful.
  virtual bool IsConnected() const = 0;

  // Calls Connect() unless IsConnected() returns true. Helper method.
  virtual Status EnsureConnected() {
    if (IsConnected()) return Status::OK();
    return Connect();
  }

  // Gets the list of Tablet Servers from the Master and stores it in the passed
  // map, which is keyed on server permanent_uuid.
  // 'tablet_servers' is only modified if this method returns OK.
  virtual Status RetrieveTabletServers(TSMap* tablet_servers) = 0;

  // Gets the list of tables from the Master and stores it in the passed vector.
  // tables is only modified if this method returns OK.
  virtual Status RetrieveTablesList(
      std::vector<std::tr1::shared_ptr<KsckTable> >* tables) = 0;

  // Gets the list of tablets for the specified table and stores the list in it.
  // The table's tablet list is only modified if this method returns OK.
  virtual Status RetrieveTabletsList(const std::tr1::shared_ptr<KsckTable>& table) = 0;

 private:
  DISALLOW_COPY_AND_ASSIGN(KsckMaster);
};

// Class used to communicate with the cluster. It bootstraps this by using the provided master.
class KsckCluster {
 public:
  explicit KsckCluster(const std::tr1::shared_ptr<KsckMaster>& master)
      : master_(master) {
  }
  ~KsckCluster();

  // Fetches list of tables, tablets, and tablet servers from the master and
  // populates the full list in cluster_->tables().
  Status FetchTableAndTabletInfo();

  const std::tr1::shared_ptr<KsckMaster>& master() {
    return master_;
  }

  const std::tr1::unordered_map<std::string,
                                std::tr1::shared_ptr<KsckTabletServer> >& tablet_servers() {
    return tablet_servers_;
  }

  const std::vector<std::tr1::shared_ptr<KsckTable> >& tables() {
    return tables_;
  }

 private:
  // Gets the list of tablet servers from the Master.
  Status RetrieveTabletServers();

  // Gets the list of tables from the Master.
  Status RetrieveTablesList();

  // Fetch the list of tablets for the given table from the Master.
  Status RetrieveTabletsList(const std::tr1::shared_ptr<KsckTable>& table);

  const std::tr1::shared_ptr<KsckMaster> master_;
  std::tr1::unordered_map<std::string, std::tr1::shared_ptr<KsckTabletServer> > tablet_servers_;
  std::vector<std::tr1::shared_ptr<KsckTable> > tables_;
  DISALLOW_COPY_AND_ASSIGN(KsckCluster);
};

// Externally facing class to run checks against the provided cluster.
class Ksck {
 public:
  explicit Ksck(const std::tr1::shared_ptr<KsckCluster>& cluster)
      : cluster_(cluster) {
  }
  ~Ksck() {}

  // Verifies that it can connect to the Master.
  Status CheckMasterRunning();

  // Populates all the cluster table and tablet info from the Master.
  Status FetchTableAndTabletInfo();

  // Verifies that it can connect to all the Tablet Servers reported by the master.
  // Must first call FetchTableAndTabletInfo().
  Status CheckTabletServersRunning();

  // Establishes a connection with the specified Tablet Server.
  // Must first call FetchTableAndTabletInfo().
  Status ConnectToTabletServer(const std::tr1::shared_ptr<KsckTabletServer>& ts);

  // Verifies that all the tables have contiguous tablets and that each tablet has enough replicas
  // and a leader.
  // Must first call FetchTableAndTabletInfo().
  Status CheckTablesConsistency();

  // Verifies data checksums on all tablets by doing a scan of the database on each replica.
  // If tables is not empty, checks only the named tables.
  // If tablets is not empty, checks only the specified tablets.
  // If both are specified, takes the intersection.
  // If both are empty, all tables and tablets are checked.
  // timeout specifies the maximum total time that the method will wait for
  // results to come back from all replicas.
  // Must first call FetchTableAndTabletInfo().
  Status ChecksumData(const std::vector<std::string>& tables,
                      const std::vector<std::string>& tablets,
                      const MonoDelta& timeout);

  // Verifies that the assignments reported by the master are the same reported by the
  // Tablet Servers.
  // Must first call FetchTableAndTabletInfo().
  Status CheckAssignments();

 private:
  bool VerifyTable(const std::tr1::shared_ptr<KsckTable>& table);
  bool VerifyTableWithTimeout(const std::tr1::shared_ptr<KsckTable>& table,
                              const MonoDelta& timeout,
                              const MonoDelta& retry_interval);
  bool VerifyTablet(const std::tr1::shared_ptr<KsckTablet>& tablet, int table_num_replicas);

  const std::tr1::shared_ptr<KsckCluster> cluster_;
  DISALLOW_COPY_AND_ASSIGN(Ksck);
};
} // namespace tools
} // namespace kudu

#endif // KUDU_TOOLS_KSCK_H
