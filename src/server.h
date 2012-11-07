/*
Minetest-c55
Copyright (C) 2010-2011 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#ifndef SERVER_HEADER
#define SERVER_HEADER

#include "connection.h"
#include "environment.h"
#include "irrlichttypes_bloated.h"
#include <string>
#include "porting.h"
#include "map.h"
#include "inventory.h"
#include "ban.h"
#include "gamedef.h"
#include "serialization.h" // For SER_FMT_VER_INVALID
#include "mods.h"
#include "inventorymanager.h"
#include "subgame.h"
#include "sound.h"
#include "util/thread.h"
#include "util/string.h"
#include "rollback_interface.h" // Needed for rollbackRevertActions()
#include <list> // Needed for rollbackRevertActions()

struct LuaState;
typedef struct lua_State lua_State;
class IWritableItemDefManager;
class IWritableNodeDefManager;
class IWritableCraftDefManager;
class EventManager;
class PlayerSAO;
class IRollbackManager;

class ServerError : public std::exception
{
public:
	ServerError(const std::string &s)
	{
		m_s = "ServerError: ";
		m_s += s;
	}
	virtual ~ServerError() throw()
	{}
	virtual const char * what() const throw()
	{
		return m_s.c_str();
	}
	std::string m_s;
};

/*
	Some random functions
*/
v3f findSpawnPos(ServerMap &map);

/*
	A structure containing the data needed for queueing the fetching
	of blocks.
*/
struct QueuedBlockEmerge
{
	v3s16 pos;
	float priority; // Larger = more important; 0 = highest
};

/*
	This is a thread-safe class.
*/
class BlockEmergeQueue
{
public:
	BlockEmergeQueue()
	{
		m_mutex.Init();
	}

	~BlockEmergeQueue()
	{
		JMutexAutoLock lock(m_mutex);

		core::list<QueuedBlockEmerge*>::Iterator i;
		for(i=m_queue.begin(); i!=m_queue.end(); i++)
		{
			QueuedBlockEmerge *q = *i;
			delete q;
		}
	}
	
	void addBlock(v3s16 pos, float priority)
	{
		DSTACK(__FUNCTION_NAME);
	
		JMutexAutoLock lock(m_mutex);
		
		// Remove from queue if it's not already queued
		for(core::list<QueuedBlockEmerge*>::Iterator
				i=m_queue.begin(); i!=m_queue.end(); i++)
		{
			QueuedBlockEmerge *q = *i;
			if(q->pos == pos){
				if(q->priority > priority){
					// Already in queue with a higher priority
					return;
				} else{
					// In queue with a lower priority; remove and re-add
					delete q;
					m_queue.erase(i);
					break;
				}
			}
		}
	
		// Add to queue

		QueuedBlockEmerge *newq = new QueuedBlockEmerge;
		newq->pos = pos;
		newq->priority = priority;

		if(m_queue.empty()){
			m_queue.push_back(newq);
		} else {
			for(core::list<QueuedBlockEmerge*>::Iterator
					i=m_queue.begin(); i!=m_queue.end(); i++)
			{
				QueuedBlockEmerge *q = *i;
				if(q->priority < priority){
					m_queue.insert_before(i, newq);
					return;
				}
			}
		}
	}

	// Returned pointer must be deleted
	// Returns NULL if queue is empty
	QueuedBlockEmerge * pop()
	{
		JMutexAutoLock lock(m_mutex);

		core::list<QueuedBlockEmerge*>::Iterator i = m_queue.begin();
		if(i == m_queue.end())
			return NULL;
		QueuedBlockEmerge *q = *i;
		m_queue.erase(i);
		return q;
	}

	u32 size()
	{
		JMutexAutoLock lock(m_mutex);
		return m_queue.size();
	}
	
private:
	// Sorted by priority; highest first
	core::list<QueuedBlockEmerge*> m_queue;
	JMutex m_mutex;
};

class Server;

class ServerThread : public SimpleThread
{
	Server *m_server;

public:

	ServerThread(Server *server):
		SimpleThread(),
		m_server(server)
	{
	}

	void * Thread();
};

class EmergeThread : public SimpleThread
{
	Server *m_server;

public:

	EmergeThread(Server *server):
		SimpleThread(),
		m_server(server)
	{
	}

	void * Thread();

	void trigger()
	{
		setRun(true);
		if(IsRunning() == false)
		{
			Start();
		}
	}
};

struct PlayerInfo
{
	u16 id;
	char name[PLAYERNAME_SIZE];
	v3f position;
	Address address;
	float avg_rtt;

	PlayerInfo();
	void PrintLine(std::ostream *s);
};

/*
	Used for queueing and sorting block transfers in containers
	
	Lower priority number means higher priority.
*/
struct PrioritySortedBlockTransfer
{
	PrioritySortedBlockTransfer(float a_priority, v3s16 a_pos, u16 a_peer_id)
	{
		priority = a_priority;
		pos = a_pos;
		peer_id = a_peer_id;
	}
	bool operator < (PrioritySortedBlockTransfer &other)
	{
		return priority < other.priority;
	}
	float priority;
	v3s16 pos;
	u16 peer_id;
};

struct MediaRequest
{
	std::string name;

	MediaRequest(const std::string &name_=""):
		name(name_)
	{}
};

struct MediaInfo
{
	std::string path;
	std::string sha1_digest;

	MediaInfo(const std::string path_="",
			const std::string sha1_digest_=""):
		path(path_),
		sha1_digest(sha1_digest_)
	{
	}
};

struct ServerSoundParams
{
	float gain;
	std::string to_player;
	enum Type{
		SSP_LOCAL=0,
		SSP_POSITIONAL=1,
		SSP_OBJECT=2
	} type;
	v3f pos;
	u16 object;
	float max_hear_distance;
	bool loop;

	ServerSoundParams():
		gain(1.0),
		to_player(""),
		type(SSP_LOCAL),
		pos(0,0,0),
		object(0),
		max_hear_distance(32*BS),
		loop(false)
	{}
	
	v3f getPos(ServerEnvironment *env, bool *pos_exists) const;
};

struct ServerPlayingSound
{
	ServerSoundParams params;
	std::set<u16> clients; // peer ids
};

struct QueuedBlockSend
{
	int peer_id;
	v3s16 pos;
	float priority; // Larger = more important; 0 = highest
	double timeout_timestamp;

	QueuedBlockSend():
		peer_id(0),
		pos(0,0,0),
		priority(0),
		timeout_timestamp(0)
	{}
};

class BlockSendQueue
{
public:
	BlockSendQueue()
	{
	}

	~BlockSendQueue()
	{
		core::list<QueuedBlockSend*>::Iterator i;
		for(i=m_queue.begin(); i!=m_queue.end(); i++)
		{
			QueuedBlockSend *q = *i;
			delete q;
		}
	}
	
	void addBlock(int peer_id, v3s16 pos, float priority, float timeout)
	{
		float timeout_timestamp = m_timestamp + timeout;
	
		// Remove from queue if it's not already queued
		for(core::list<QueuedBlockSend*>::Iterator
				i=m_queue.begin(); i!=m_queue.end(); i++)
		{
			QueuedBlockSend *q = *i;
			if(q->peer_id == peer_id && q->pos == pos){
				if(q->priority > priority && q->timeout_timestamp > timeout_timestamp){
					// Already in queue with a higher priority and higher timoeut
					return;
				} else{
					// In queue with a lower priority; remove and re-add
					delete q;
					m_queue.erase(i);
					break;
				}
			}
		}
	
		// Add to queue

		QueuedBlockSend *newq = new QueuedBlockSend;
		newq->peer_id = peer_id;
		newq->pos = pos;
		newq->priority = priority;
		newq->timeout_timestamp = timeout_timestamp;

		if(m_queue.empty()){
			m_queue.push_back(newq);
		} else {
			for(core::list<QueuedBlockSend*>::Iterator
					i=m_queue.begin(); i!=m_queue.end(); i++)
			{
				QueuedBlockSend *q = *i;
				if(q->priority < priority){
					m_queue.insert_before(i, newq);
					return;
				}
			}
		}
	}

	void step(double dtime)
	{
		m_timestamp += dtime;
	}

	u32 size()
	{
		return m_queue.size();
	}
	
	// FIXME: This shouldn't require the server at all; the data should be fed
	// in addBlock and only the connection should be needed here
	void send(Server &server, float packet_queue_max_seconds);

private:
	// Sorted by priority; highest first
	core::list<QueuedBlockSend*> m_queue;
	double m_timestamp;
};

class RemoteClient
{
public:
	// peer_id=0 means this client has no associated peer
	// NOTE: If client is made allowed to exist while peer doesn't,
	//       this has to be set to 0 when there is no peer.
	//       Also, the client must be moved to some other container.
	u16 peer_id;
	// The serialization version to use with the client
	u8 serialization_version;
	//
	u16 net_proto_version;
	// Version is stored in here after INIT before INIT2
	u8 pending_serialization_version;

	bool definitions_sent;

	RemoteClient():
		m_time_from_building(9999)
	{
		peer_id = 0;
		serialization_version = SER_FMT_VER_INVALID;
		net_proto_version = 0;
		pending_serialization_version = SER_FMT_VER_INVALID;
		definitions_sent = false;
		m_nearest_unsent_d = 0;
		m_nearest_unsent_reset_timer = 0.0;
		m_nothing_to_send_counter = 0;
		m_nothing_to_send_pause_timer = 0;
	}
	~RemoteClient()
	{
	}
	
	// Increments timeouts and removes timed-out blocks from list
	// NOTE: This doesn't fix the server-not-sending-block bug
	//       because it is related to emerging, not sending.
	//void RunSendingTimeouts(float dtime, float timeout);

	void PrintInfo(std::ostream &o)
	{
		o<<"RemoteClient "<<peer_id<<": "
				<<", m_nearest_unsent_d="<<m_nearest_unsent_d
				<<std::endl;
	}

	// Time from last placing or removing blocks
	float m_time_from_building;
	
	/*JMutex m_dig_mutex;
	float m_dig_time_remaining;
	// -1 = not digging
	s16 m_dig_tool_item;
	v3s16 m_dig_position;*/
	
	/*
		List of active objects that the client knows of.
		Value is dummy.
	*/
	core::map<u16, bool> m_known_objects;

private:
	/*
		Blocks that have been sent to client.
		- These don't have to be sent again.
		- A block is cleared from here when client says it has
		  deleted it from it's memory
		
		Key is position, value is dummy.
		No MapBlock* is stored here because the blocks can get deleted.
	*/
	core::map<v3s16, bool> m_blocks_sent;
	s16 m_nearest_unsent_d;
	v3s16 m_last_center;
	float m_nearest_unsent_reset_timer;
	
	// CPU usage optimization
	u32 m_nothing_to_send_counter;
	float m_nothing_to_send_pause_timer;
};

class Server : public con::PeerHandler, public MapEventReceiver,
		public InventoryManager, public IGameDef
{
public:
	/*
		NOTE: Every public method should be thread-safe
	*/
	
	Server(
		const std::string &path_world,
		const std::string &path_config,
		const SubgameSpec &gamespec,
		bool simple_singleplayer_mode
	);
	~Server();
	void start(unsigned short port);
	void stop();
	// This is mainly a way to pass the time to the server.
	// Actual processing is done in an another thread.
	void step(float dtime);
	// This is run by ServerThread and does the actual processing
	void AsyncRunStep();
	void Receive();
	void ProcessData(u8 *data, u32 datasize, u16 peer_id);

	core::list<PlayerInfo> getPlayerInfo();

	// Environment must be locked when called
	void setTimeOfDay(u32 time)
	{
		m_env->setTimeOfDay(time);
		m_time_of_day_send_timer = 0;
	}

	bool getShutdownRequested()
	{
		return m_shutdown_requested;
	}
	
	/*
		Shall be called with the environment locked.
		This is accessed by the map, which is inside the environment,
		so it shouldn't be a problem.
	*/
	void onMapEditEvent(MapEditEvent *event);

	/*
		Shall be called with the environment and the connection locked.
	*/
	Inventory* getInventory(const InventoryLocation &loc);
	void setInventoryModified(const InventoryLocation &loc);

	// Connection must be locked when called
	std::wstring getStatusString();

	void requestShutdown(void)
	{
		m_shutdown_requested = true;
	}

	// Returns -1 if failed, sound handle on success
	// Envlock + conlock
	s32 playSound(const SimpleSoundSpec &spec, const ServerSoundParams &params);
	void stopSound(s32 handle);
	
	// Envlock + conlock
	std::set<std::string> getPlayerEffectivePrivs(const std::string &name);
	bool checkPriv(const std::string &name, const std::string &priv);
	void reportPrivsModified(const std::string &name=""); // ""=all
	void reportInventoryFormspecModified(const std::string &name);

	// Saves g_settings to configpath given at initialization
	void saveConfig();

	void setIpBanned(const std::string &ip, const std::string &name)
	{
		m_banmanager.add(ip, name);
		return;
	}

	void unsetIpBanned(const std::string &ip_or_name)
	{
		m_banmanager.remove(ip_or_name);
		return;
	}

	std::string getBanDescription(const std::string &ip_or_name)
	{
		return m_banmanager.getBanDescription(ip_or_name);
	}

	Address getPeerAddress(u16 peer_id)
	{
		return m_con.GetPeerAddress(peer_id);
	}
	
	// Envlock and conlock should be locked when calling this
	void notifyPlayer(const char *name, const std::wstring msg);
	void notifyPlayers(const std::wstring msg);

	// Creates or resets inventory
	Inventory* createDetachedInventory(const std::string &name);
	
	// Envlock and conlock should be locked when using Lua
	lua_State *getLua(){ return m_lua; }

	// Envlock should be locked when using the rollback manager
	IRollbackManager *getRollbackManager(){ return m_rollback; }
	// actions: time-reversed list
	// Return value: success/failure
	bool rollbackRevertActions(const std::list<RollbackAction> &actions,
			std::list<std::string> *log);
	
	// IGameDef interface
	// Under envlock
	virtual IItemDefManager* getItemDefManager();
	virtual INodeDefManager* getNodeDefManager();
	virtual ICraftDefManager* getCraftDefManager();
	virtual ITextureSource* getTextureSource();
	virtual u16 allocateUnknownNodeId(const std::string &name);
	virtual ISoundManager* getSoundManager();
	virtual MtEventManager* getEventManager();
	virtual IRollbackReportSink* getRollbackReportSink();
	
	IWritableItemDefManager* getWritableItemDefManager();
	IWritableNodeDefManager* getWritableNodeDefManager();
	IWritableCraftDefManager* getWritableCraftDefManager();

	const ModSpec* getModSpec(const std::string &modname);
	void getModNames(core::list<std::string> &modlist);
	std::string getBuiltinLuaPath();
	
	std::string getWorldPath(){ return m_path_world; }

	bool isSingleplayer(){ return m_simple_singleplayer_mode; }

	void setAsyncFatalError(const std::string &error)
	{
		m_async_fatal_error.set(error);
	}

private:

	// con::PeerHandler implementation.
	// These queue stuff to be processed by handlePeerChanges().
	// As of now, these create and remove clients and players.
	void peerAdded(con::Peer *peer);
	void deletingPeer(con::Peer *peer, bool timeout);
	
	/*
		Static send methods
	*/
	
	static void SendHP(con::Connection &con, u16 peer_id, u8 hp);
	static void SendAccessDenied(con::Connection &con, u16 peer_id,
			const std::wstring &reason);
	static void SendDeathscreen(con::Connection &con, u16 peer_id,
			bool set_camera_point_target, v3f camera_point_target);
	static void SendItemDef(con::Connection &con, u16 peer_id,
			IItemDefManager *itemdef);
	static void SendNodeDef(con::Connection &con, u16 peer_id,
			INodeDefManager *nodedef);
	
	/*
		Non-static send methods.
		Conlock should be always used.
		Envlock usage is documented badly but it's easy to figure out
		which ones access the environment.
	*/

	// Envlock and conlock should be locked when calling these
	void SendInventory(u16 peer_id);
	void SendChatMessage(u16 peer_id, const std::wstring &message);
	void BroadcastChatMessage(const std::wstring &message);
	void SendPlayerHP(u16 peer_id);
	void SendMovePlayer(u16 peer_id);
	void SendPlayerPrivileges(u16 peer_id);
	void SendPlayerInventoryFormspec(u16 peer_id);
	/*
		Send a node removal/addition event to all clients except ignore_id.
		Additionally, if far_players!=NULL, players further away than
		far_d_nodes are ignored and their peer_ids are added to far_players
	*/
	// Envlock and conlock should be locked when calling these
	void sendRemoveNode(v3s16 p, u16 ignore_id=0, float far_d_nodes=100);
	void sendAddNode(v3s16 p, MapNode n, u16 ignore_id=0, float far_d_nodes=100);
	
	// Environment and Connection must be locked when called
	void SendBlockNoLock(u16 peer_id, MapBlock *block, u8 ver);
	
	void fillMediaCache();
	void sendMediaAnnouncement(u16 peer_id);
	void sendRequestedMedia(u16 peer_id,
			const core::list<MediaRequest> &tosend);
	
	void sendDetachedInventory(const std::string &name, u16 peer_id);
	void sendDetachedInventoryToAll(const std::string &name);
	void sendDetachedInventories(u16 peer_id);

	/*
		Something random
	*/
	
	void DiePlayer(u16 peer_id);
	void RespawnPlayer(u16 peer_id);
	
	void UpdateCrafting(u16 peer_id);
	
	// When called, connection mutex should be locked
	RemoteClient* getClient(u16 peer_id);
	
	// When called, environment mutex should be locked
	std::string getPlayerName(u16 peer_id)
	{
		Player *player = m_env->getPlayer(peer_id);
		if(player == NULL)
			return "[id="+itos(peer_id)+"]";
		return player->getName();
	}

	// When called, environment mutex should be locked
	PlayerSAO* getPlayerSAO(u16 peer_id)
	{
		Player *player = m_env->getPlayer(peer_id);
		if(player == NULL)
			return NULL;
		return player->getPlayerSAO();
	}

	/*
		Get a player from memory or creates one.
		If player is already connected, return NULL
		Does not verify/modify auth info and password.

		Call with env and con locked.
	*/
	PlayerSAO *emergePlayer(const char *name, u16 peer_id);
	
	// Locks environment and connection by its own
	struct PeerChange;
	void handlePeerChange(PeerChange &c);
	void handlePeerChanges();

	/*
		Variables
	*/
	
	// World directory
	std::string m_path_world;
	// Path to user's configuration file ("" = no configuration file)
	std::string m_path_config;
	// Subgame specification
	SubgameSpec m_gamespec;
	// If true, do not allow multiple players and hide some multiplayer
	// functionality
	bool m_simple_singleplayer_mode;

	// Thread can set; step() will throw as ServerError
	MutexedVariable<std::string> m_async_fatal_error;
	
	// Some timers
	float m_liquid_transform_timer;
	float m_print_info_timer;
	float m_objectdata_timer;
	float m_emergethread_trigger_timer;
	float m_savemap_timer;
	IntervalLimiter m_map_timer_and_unload_interval;
	
	// NOTE: If connection and environment are both to be locked,
	// environment shall be locked first.

	// Environment
	ServerEnvironment *m_env;
	JMutex m_env_mutex;
	
	// Connection
	con::Connection m_con;
	JMutex m_con_mutex;
	// Connected clients (behind the con mutex)
	core::map<u16, RemoteClient*> m_clients;

	// Bann checking
	BanManager m_banmanager;

	// Rollback manager (behind m_env_mutex)
	IRollbackManager *m_rollback;
	bool m_rollback_sink_enabled;
	bool m_enable_rollback_recording; // Updated once in a while

	// Scripting
	// Envlock and conlock should be locked when using Lua
	lua_State *m_lua;

	// Item definition manager
	IWritableItemDefManager *m_itemdef;
	
	// Node definition manager
	IWritableNodeDefManager *m_nodedef;
	
	// Craft definition manager
	IWritableCraftDefManager *m_craftdef;
	
	// Event manager
	EventManager *m_event;
	
	// Mods
	core::list<ModSpec> m_mods;
	
	/*
		Threads
	*/
	
	// A buffer for time steps
	// step() increments and AsyncRunStep() run by m_thread reads it.
	float m_step_dtime;
	JMutex m_step_dtime_mutex;

	// The server mainly operates in this thread
	ServerThread m_thread;
	// This thread fetches and generates map
	EmergeThread m_emergethread;
	// Queue of block coordinates to be processed by the emerge thread
	BlockEmergeQueue m_emerge_queue;
	
	/*
		Time related stuff
	*/

	// Timer for sending time of day over network
	float m_time_of_day_send_timer;
	// Uptime of server in seconds
	MutexedVariable<double> m_uptime;
	
	/*
		Peer change queue.
		Queues stuff from peerAdded() and deletingPeer() to
		handlePeerChanges()
	*/
	enum PeerChangeType
	{
		PEER_ADDED,
		PEER_REMOVED
	};
	struct PeerChange
	{
		PeerChangeType type;
		u16 peer_id;
		bool timeout;
	};
	Queue<PeerChange> m_peer_change_queue;

	/*
		Random stuff
	*/
	
	// Mod parent directory paths
	core::list<std::string> m_modspaths;

	bool m_shutdown_requested;

	/*
		Map edit event queue. Automatically receives all map edits.
		The constructor of this class registers us to receive them through
		onMapEditEvent

		NOTE: Should these be moved to actually be members of
		ServerEnvironment?
	*/

	/*
		Queue of map edits from the environment for sending to the clients
		This is behind m_env_mutex
	*/
	Queue<MapEditEvent*> m_unsent_map_edit_queue;
	/*
		Set to true when the server itself is modifying the map and does
		all sending of information by itself.
		This is behind m_env_mutex
	*/
	bool m_ignore_map_edit_events;
	/*
		If a non-empty area, map edit events contained within are left
		unsent. Done at map generation time to speed up editing of the
		generated area, as it will be sent anyway.
		This is behind m_env_mutex
	*/
	VoxelArea m_ignore_map_edit_events_area;
	/*
		If set to !=0, the incoming MapEditEvents are modified to have
		this peed id as the disabled recipient
		This is behind m_env_mutex
	*/
	u16 m_ignore_map_edit_events_peer_id;

	friend class EmergeThread;
	friend class RemoteClient;
	friend class BlockSendQueue;

	std::map<std::string,MediaInfo> m_media;

	/*
		Sounds
	*/
	std::map<s32, ServerPlayingSound> m_playing_sounds;
	s32 m_next_sound_id;

	/*
		Detached inventories (behind m_env_mutex)
	*/
	// key = name
	std::map<std::string, Inventory*> m_detached_inventories;

	/* Block send queue */
	BlockSendQueue m_block_send_queue;
};

/*
	Runs a simple dedicated server loop.

	Shuts down when run is set to false.
*/
void dedicated_server_loop(Server &server, bool &run);

#endif

