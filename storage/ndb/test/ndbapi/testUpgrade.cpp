/* Copyright (C) 2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <NDBT.hpp>
#include <NDBT_Test.hpp>
#include <HugoTransactions.hpp>
#include <UtilTransactions.hpp>
#include <NdbRestarter.hpp>
#include <AtrtClient.hpp>
#include <Bitmask.hpp>

struct NodeInfo
{
  int nodeId;
  int processId;
  int nodeGroup;
};

/**
  Test that one node at a time can be upgraded
*/

int runUpgrade_NR1(NDBT_Context* ctx, NDBT_Step* step){
  AtrtClient atrt;

  SqlResultSet clusters;
  if (!atrt.getClusters(clusters))
    return NDBT_FAILED;

  while (clusters.next())
  {
    uint clusterId= clusters.columnAsInt("id");
    SqlResultSet tmp_result;
    if (!atrt.getConnectString(clusterId, tmp_result))
      return NDBT_FAILED;

    NdbRestarter restarter(tmp_result.column("connectstring"));
    restarter.setReconnect(true); // Restarting mgmd
    g_err << "Cluster '" << clusters.column("name")
          << "@" << tmp_result.column("connectstring") << "'" << endl;

    if (restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart ndb_mgmd(s)
    SqlResultSet mgmds;
    if (!atrt.getMgmds(clusterId, mgmds))
      return NDBT_FAILED;

    while (mgmds.next())
    {
      ndbout << "Restart mgmd " << mgmds.columnAsInt("node_id") << endl;
      if (!atrt.changeVersion(mgmds.columnAsInt("id"), ""))
        return NDBT_FAILED;

      if (restarter.waitConnected())
        return NDBT_FAILED;
      ndbout << "Connected to mgmd"<< endl;
    }

    ndbout << "Waiting for started"<< endl;
    if (restarter.waitClusterStarted())
      return NDBT_FAILED;
    ndbout << "Started"<< endl;

    // Restart ndbd(s)
    SqlResultSet ndbds;
    if (!atrt.getNdbds(clusterId, ndbds))
      return NDBT_FAILED;

    while(ndbds.next())
    {
      int nodeId = ndbds.columnAsInt("node_id");
      int processId = ndbds.columnAsInt("id");
      ndbout << "Restart node " << nodeId << endl;

      if (!atrt.changeVersion(processId, ""))
        return NDBT_FAILED;

      if (restarter.waitNodesNoStart(&nodeId, 1))
        return NDBT_FAILED;

      if (restarter.startNodes(&nodeId, 1))
        return NDBT_FAILED;

      if (restarter.waitNodesStarted(&nodeId, 1))
        return NDBT_FAILED;

    }
  }

  ctx->stopTest();
  return NDBT_OK;
}


/**
   Test that one node in each nodegroup can be upgraded simultaneously
    - using method1
*/

int runUpgrade_NR2(NDBT_Context* ctx, NDBT_Step* step){
  // Assuming 2 replicas

  AtrtClient atrt;

  SqlResultSet clusters;
  if (!atrt.getClusters(clusters))
    return NDBT_FAILED;

  while (clusters.next())
  {
    uint clusterId= clusters.columnAsInt("id");
    SqlResultSet tmp_result;
    if (!atrt.getConnectString(clusterId, tmp_result))
      return NDBT_FAILED;

    NdbRestarter restarter(tmp_result.column("connectstring"));
    restarter.setReconnect(true); // Restarting mgmd
    g_err << "Cluster '" << clusters.column("name")
          << "@" << tmp_result.column("connectstring") << "'" << endl;

    if(restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart ndb_mgmd(s)
    SqlResultSet mgmds;
    if (!atrt.getMgmds(clusterId, mgmds))
      return NDBT_FAILED;

    while (mgmds.next())
    {
      ndbout << "Restart mgmd" << mgmds.columnAsInt("node_id") << endl;
      if (!atrt.changeVersion(mgmds.columnAsInt("id"), ""))
        return NDBT_FAILED;

      if(restarter.waitConnected())
        return NDBT_FAILED;
    }

    NdbSleep_SecSleep(5); // TODO, handle arbitration

    // Restart one ndbd in each node group
    SqlResultSet ndbds;
    if (!atrt.getNdbds(clusterId, ndbds))
      return NDBT_FAILED;

    Vector<NodeInfo> nodes;
    while (ndbds.next())
    {
      struct NodeInfo n;
      n.nodeId = ndbds.columnAsInt("node_id");
      n.processId = ndbds.columnAsInt("id");
      n.nodeGroup = restarter.getNodeGroup(n.nodeId);
      nodes.push_back(n);
    }

    Bitmask<4> seen_groups;
    Bitmask<4> restarted_nodes;
    for (Uint32 i = 0; i<nodes.size(); i++)
    {
      int nodeId = nodes[i].nodeId;
      int processId = nodes[i].processId;
      int nodeGroup= nodes[i].nodeGroup;

      if (seen_groups.get(nodeGroup))
      {
        // One node in this node group already down
        continue;
      }
      seen_groups.set(nodeGroup);
      restarted_nodes.set(nodeId);

      ndbout << "Restart node " << nodeId << endl;

      if (!atrt.changeVersion(processId, ""))
        return NDBT_FAILED;

      if (restarter.waitNodesNoStart(&nodeId, 1))
        return NDBT_FAILED;

    }

    ndbout << "Starting and wait for started..." << endl;
    if (restarter.startAll())
      return NDBT_FAILED;

    if (restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart the remaining nodes
    for (Uint32 i = 0; i<nodes.size(); i++)
    {
      int nodeId = nodes[i].nodeId;
      if (restarted_nodes.get(nodeId))
        continue;
      
      int processId = nodes[i].processId;
      ndbout << "Restart node " << nodeId << endl;
      if (!atrt.changeVersion(processId, ""))
        return NDBT_FAILED;
      
      if (restarter.waitNodesNoStart(&nodeId, 1))
        return NDBT_FAILED;
      
    }
    
    ndbout << "Starting and wait for started..." << endl;
    if (restarter.startAll())
      return NDBT_FAILED;

    if (restarter.waitClusterStarted())
      return NDBT_FAILED;

  }

  ctx->stopTest();
  return NDBT_OK;
}


/**
   Test that one node in each nodegroup can be upgrade simultaneously
    - using method2, ie. don't wait for "nostart" before stopping
      next node
*/

int runUpgrade_NR3(NDBT_Context* ctx, NDBT_Step* step){
  // Assuming 2 replicas

  AtrtClient atrt;

  SqlResultSet clusters;
  if (!atrt.getClusters(clusters))
    return NDBT_FAILED;

  while (clusters.next())
  {
    uint clusterId= clusters.columnAsInt("id");
    SqlResultSet tmp_result;
    if (!atrt.getConnectString(clusterId, tmp_result))
      return NDBT_FAILED;

    NdbRestarter restarter(tmp_result.column("connectstring"));
    restarter.setReconnect(true); // Restarting mgmd
    g_err << "Cluster '" << clusters.column("name")
          << "@" << tmp_result.column("connectstring") << "'" << endl;

    if(restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart ndb_mgmd(s)
    SqlResultSet mgmds;
    if (!atrt.getMgmds(clusterId, mgmds))
      return NDBT_FAILED;

    while (mgmds.next())
    {
      ndbout << "Restart mgmd" << mgmds.columnAsInt("node_id") << endl;
      if (!atrt.changeVersion(mgmds.columnAsInt("id"), ""))
        return NDBT_FAILED;

      if(restarter.waitConnected())
        return NDBT_FAILED;
    }

    NdbSleep_SecSleep(5); // TODO, handle arbitration

    // Restart one ndbd in each node group
    SqlResultSet ndbds;
    if (!atrt.getNdbds(clusterId, ndbds))
      return NDBT_FAILED;

    Vector<NodeInfo> nodes;
    while (ndbds.next())
    {
      struct NodeInfo n;
      n.nodeId = ndbds.columnAsInt("node_id");
      n.processId = ndbds.columnAsInt("id");
      n.nodeGroup = restarter.getNodeGroup(n.nodeId);
      nodes.push_back(n);
    }

    int nodesarray[256];
    int cnt= 0;

    Bitmask<4> seen_groups;
    Bitmask<4> restarted_nodes;
    for (Uint32 i = 0; i<nodes.size(); i++)
    {
      int nodeId = nodes[i].nodeId;
      int processId = nodes[i].processId;
      int nodeGroup= nodes[i].nodeGroup;

      if (seen_groups.get(nodeGroup))
      {
        // One node in this node group already down
        continue;
      }
      seen_groups.set(nodeGroup);
      restarted_nodes.set(nodeId);

      ndbout << "Restart node " << nodeId << endl;
      
      if (!atrt.changeVersion(processId, ""))
        return NDBT_FAILED;
      
      nodesarray[cnt++]= nodeId;
    }
    
    if (restarter.waitNodesNoStart(nodesarray, cnt))
      return NDBT_FAILED;
    
    ndbout << "Starting and wait for started..." << endl;
    if (restarter.startAll())
      return NDBT_FAILED;

    if (restarter.waitClusterStarted())
      return NDBT_FAILED;

    // Restart the remaining nodes
    cnt= 0;
    for (Uint32 i = 0; i<nodes.size(); i++)
    {
      int nodeId = nodes[i].nodeId;
      int processId = nodes[i].processId;

      if (restarted_nodes.get(nodeId))
        continue;
      
      ndbout << "Restart node " << nodeId << endl;
      if (!atrt.changeVersion(processId, ""))
        return NDBT_FAILED;

      nodesarray[cnt++]= nodeId;
    }
    
    if (restarter.waitNodesNoStart(nodesarray, cnt))
      return NDBT_FAILED;
    
    
    ndbout << "Starting and wait for started..." << endl;
    if (restarter.startAll())
      return NDBT_FAILED;
    
    if (restarter.waitClusterStarted())
      return NDBT_FAILED;
  }

  ctx->stopTest();
  return NDBT_OK;
}


int runCheckStarted(NDBT_Context* ctx, NDBT_Step* step){

  // Check cluster is started
  NdbRestarter restarter;
  if(restarter.waitClusterStarted() != 0){
    g_err << "All nodes was not started " << endl;
    return NDBT_FAILED;
  }

  // Check atrtclient is started
  AtrtClient atrt;
  if(!atrt.waitConnected()){
    g_err << "atrt server was not started " << endl;
    return NDBT_FAILED;
  }

  // Make sure atrt assigns nodeid != -1
  SqlResultSet procs;
  if (!atrt.doQuery("SELECT * FROM process", procs))
    return NDBT_FAILED;

  while (procs.next())
  {
    if (procs.columnAsInt("node_id") == (unsigned)-1){
      ndbout << "Found one process with node_id -1, "
             << "use --fix-nodeid=1 to atrt to fix this" << endl;
      return NDBT_FAILED;
    }
  }

  return NDBT_OK;
}

NDBT_TESTSUITE(testUpgrade);
TESTCASE("Upgrade_NR1",
	 "Test that one node at a time can be upgraded"){
  INITIALIZER(runCheckStarted);
  STEP(runUpgrade_NR1);
}
TESTCASE("Upgrade_NR2",
	 "Test that one node in each nodegroup can be upgradde simultaneously"){
  INITIALIZER(runCheckStarted);
  STEP(runUpgrade_NR2);
}
TESTCASE("Upgrade_NR3",
	 "Test that one node in each nodegroup can be upgrade simultaneously"){
  INITIALIZER(runCheckStarted);
  STEP(runUpgrade_NR3);
}
NDBT_TESTSUITE_END(testUpgrade);

int main(int argc, const char** argv){
  ndb_init();
  NDBT_TESTSUITE_INSTANCE(testUpgrade);
  testUpgrade.setCreateAllTables(true);
  return testUpgrade.execute(argc, argv);
}

template class Vector<NodeInfo>;
