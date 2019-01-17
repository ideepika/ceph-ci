// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include "rgw_amqp.h"
#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <amqp_framing.h>
#include "include/ceph_assert.h"
#include <sstream>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <boost/lockfree/queue.hpp>

// TODO investigation, not necessarily issues:
// (1) in case of single threaded writer contex use spsc_queue
// (2) support multiple channels
// (3) check performance of emptying queue to local list, and go over the list and publish
// (4) use std::shared_mutex (c++17) or eqivalent for the connections lock

namespace rgw::amqp {

// key class for the connection list
struct connection_id_t {
  std::string host;
  int port;
  std::string vhost;
  // constructed from amqp_connection_info struct
  connection_id_t(const amqp_connection_info& info) 
    : host(info.host), port(info.port), vhost(info.vhost) {}

  // equality operator and hasher functor are needed 
  // so that connection_id_t could be used as key in unordered_map
  bool operator==(const connection_id_t& other) const {
    return host == other.host && port == other.port && vhost == other.vhost;
  }
  
  struct hasher {
    std::size_t operator()(const connection_id_t& k) const {
       return ((std::hash<std::string>()(k.host)
             ^ (std::hash<int>()(k.port) << 1)) >> 1)
             ^ (std::hash<std::string>()(k.vhost) << 1); 
    }
  };
};

// struct for holding the connection state object
// as well as the exchange
struct connection_t {
  amqp_connection_state_t state;
  const std::string exchange;
  const amqp_bytes_t reply_to_queue;
  bool mark_for_deletion;
  uint64_t delivery_tag;
  
  connection_t(amqp_connection_state_t _state, 
      const std::string _exchange,
      const amqp_bytes_t& _reply_to_queue) :
    state(_state), 
    exchange(_exchange), 
    reply_to_queue(amqp_bytes_malloc_dup(_reply_to_queue)),
    mark_for_deletion(false),
    delivery_tag(1) {}

    // an explicit destructor, to be used only when
    // the connection is removed fro mthe list
    void destroy() {
      amqp_destroy_connection(state);
      state = nullptr;
      amqp_bytes_free(reply_to_queue);
    }
};

// convert connection info to string
std::string to_string(const amqp_connection_info& info) {
  std::stringstream ss;
  ss << "connection info:" <<
        "\nHost: " << info.host <<
        "\nPort: " << info.port <<
        "\nUser: " << info.user << 
        "\nPassword: " << info.password <<
        "\nvhost: " << info.vhost <<
        "\nSSL support: " << info.ssl << std::endl;
  return ss.str();
}

// convert error reply to string
std::string to_string(const amqp_rpc_reply_t& reply) {
  std::stringstream ss;
  switch (reply.reply_type) {
    case AMQP_RESPONSE_NORMAL:
      return "";
    case AMQP_RESPONSE_NONE:
      return "missing RPC reply type";
    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
      return amqp_error_string2(reply.library_error);
    case AMQP_RESPONSE_SERVER_EXCEPTION: 
      {
        switch (reply.reply.id) {
          case AMQP_CONNECTION_CLOSE_METHOD:
            ss << "server connection error: ";
            break;
          case AMQP_CHANNEL_CLOSE_METHOD:
            ss << "server channel error: ";
            break;
          default:
            ss << "server unknown error: ";
            break;
        }
        if (reply.reply.decoded) {
          amqp_connection_close_t* m = (amqp_connection_close_t*)reply.reply.decoded;
          ss << m->reply_code << " text: " << std::string((char*)m->reply_text.bytes, m->reply_text.len);
        }
        return ss.str();
      }
    default:
      ss << "unknown error, method id: " << reply.reply.id;
      return ss.str();
  }
}

// convert status enum to string
std::string to_string(amqp_status_enum s) {
  switch (s) {
    case AMQP_STATUS_OK:
      return "AMQP_STATUS_OK";
    case AMQP_STATUS_NO_MEMORY:
      return "AMQP_STATUS_NO_MEMORY";
    case AMQP_STATUS_BAD_AMQP_DATA:
      return "AMQP_STATUS_BAD_AMQP_DATA";
    case AMQP_STATUS_UNKNOWN_CLASS:
      return "AMQP_STATUS_UNKNOWN_CLASS";
    case AMQP_STATUS_UNKNOWN_METHOD:
      return "AMQP_STATUS_UNKNOWN_METHOD";
    case AMQP_STATUS_HOSTNAME_RESOLUTION_FAILED:
      return "AMQP_STATUS_HOSTNAME_RESOLUTION_FAILED";
    case AMQP_STATUS_INCOMPATIBLE_AMQP_VERSION:
      return "AMQP_STATUS_INCOMPATIBLE_AMQP_VERSION";
    case AMQP_STATUS_CONNECTION_CLOSED:
      return "AMQP_STATUS_CONNECTION_CLOSED";
    case AMQP_STATUS_BAD_URL:
      return "AMQP_STATUS_BAD_URL";
    case AMQP_STATUS_SOCKET_ERROR:
      return "AMQP_STATUS_SOCKET_ERROR: " + std::to_string(errno);
    case AMQP_STATUS_INVALID_PARAMETER:
      return "AMQP_STATUS_INVALID_PARAMETER"; 
    case AMQP_STATUS_TABLE_TOO_BIG:
      return "AMQP_STATUS_TABLE_TOO_BIG";
    case AMQP_STATUS_WRONG_METHOD:
      return "AMQP_STATUS_WRONG_METHOD";
    case AMQP_STATUS_TIMEOUT:
      return "AMQP_STATUS_TIMEOUT";
    case AMQP_STATUS_TIMER_FAILURE:
      return "AMQP_STATUS_TIMER_FAILURE";
    case AMQP_STATUS_HEARTBEAT_TIMEOUT:
      return "AMQP_STATUS_HEARTBEAT_TIMEOUT";
    case AMQP_STATUS_UNEXPECTED_STATE:
      return "AMQP_STATUS_UNEXPECTED_STATE";
    case AMQP_STATUS_SOCKET_CLOSED:
      return "AMQP_STATUS_SOCKET_CLOSED";
    case AMQP_STATUS_SOCKET_INUSE:
      return "AMQP_STATUS_SOCKET_INUSE";
    case AMQP_STATUS_BROKER_UNSUPPORTED_SASL_METHOD:
      return "AMQP_STATUS_BROKER_UNSUPPORTED_SASL_METHOD";
#if AMQP_VERSION >= AMQP_VERSION_CODE(0, 8, 0, 0)
    case AMQP_STATUS_UNSUPPORTED:
      return "AMQP_STATUS_UNSUPPORTED";
#endif
    case _AMQP_STATUS_NEXT_VALUE:
      return "AMQP_STATUS_INTERNAL"; 
    case AMQP_STATUS_TCP_ERROR:
        return "AMQP_STATUS_TCP_ERROR: " + std::to_string(errno);
    case AMQP_STATUS_TCP_SOCKETLIB_INIT_ERROR:
      return "AMQP_STATUS_TCP_SOCKETLIB_INIT_ERROR";
    case _AMQP_STATUS_TCP_NEXT_VALUE:
      return "AMQP_STATUS_INTERNAL"; 
    case AMQP_STATUS_SSL_ERROR:
      return "AMQP_STATUS_SSL_ERROR";
    case AMQP_STATUS_SSL_HOSTNAME_VERIFY_FAILED:
      return "AMQP_STATUS_SSL_HOSTNAME_VERIFY_FAILED";
    case AMQP_STATUS_SSL_PEER_VERIFY_FAILED:
      return "AMQP_STATUS_SSL_PEER_VERIFY_FAILED";
    case AMQP_STATUS_SSL_CONNECTION_FAILED:
      return "AMQP_STATUS_SSL_CONNECTION_FAILED";
    case _AMQP_STATUS_SSL_NEXT_VALUE:
      return "AMQP_STATUS_INTERNAL"; 
  }
  return "AMQP_STATUS_UNKNOWN";
}

// RGW AMQP status codes
static const int RGW_AMQP_STATUS_BROKER_NACK = -1000;
static const int RGW_AMQP_STATUS_CONNECTION_CLOSED = -1001;
static const int RGW_AMQP_STATUS_QUEUE_FULL = -1002;
static const int RGW_AMQP_STATUS_MAX_INFLIGHT = -1003;

// convert int status to string - including RGW specific values
std::string status_to_string(int s) {
  if (s == RGW_AMQP_STATUS_BROKER_NACK) {
    return "RGW_AMQP_STATUS_BROKER_NACK";
  } else if (s == RGW_AMQP_STATUS_CONNECTION_CLOSED) {
    return "RGW_AMQP_STATUS_CONNECTION_CLOSED";
  } else if (s == RGW_AMQP_STATUS_QUEUE_FULL) {
    return "RGW_AMQP_STATUS_QUEUE_FULL";
  } else if (s == RGW_AMQP_STATUS_MAX_INFLIGHT) {
    return "RGW_AMQP_STATUS_MAX_INFLIGHT";
  }
  return to_string((amqp_status_enum)s);
}

// in case of RPC calls, getting ther RPC reply and throwing if an error detected
void throw_on_reply_error(amqp_connection_state_t conn, const std::string& prefix_text) {
    const auto reply = amqp_get_rpc_reply(conn);
    if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
      throw(connection_error(prefix_text + to_string(reply)));
    }
}

// connection_t state cleaner
// could be used for automatic cleanup when getting out of scope
class ConnectionCleaner {
  private:
    amqp_connection_state_t conn;
  public:
    ConnectionCleaner(amqp_connection_state_t _conn) : conn(_conn) {}
    ~ConnectionCleaner() {
      if (conn) {
        amqp_destroy_connection(conn);
      }
    }
    // call reset() if cleanup is not needed anymore
    void reset() {
      conn = nullptr;
    }
};

static const amqp_channel_t CHANNEL_ID = 1;
static const amqp_channel_t CONFIRMING_CHANNEL_ID = 2;

// utility function to create a connection
// TODO: retry in case an error is recoverable
connection_t create_connection(const amqp_connection_info& info, 
    const std::string& exchange) { 
  // create connection state
  auto conn = amqp_new_connection();
  ConnectionCleaner guard(conn);
  if (!conn) {
    throw (connection_error("failed to allocate connection"));
  }

  // create and open socket
  auto socket = amqp_tcp_socket_new(conn);
  if (!socket) {
    throw(connection_error("failed to allocate socket"));
  }
  const auto s = amqp_socket_open(socket, info.host, info.port);
  if (s < 0) {
    throw(connection_error("failed to connect to broker: " + to_string((amqp_status_enum)s)));
  }

  // login to broker
  const auto reply = amqp_login(conn,
      info.vhost, 
      AMQP_DEFAULT_MAX_CHANNELS,
      AMQP_DEFAULT_FRAME_SIZE,
      0,                        // no heartbeat TODO: add conf
      AMQP_SASL_METHOD_PLAIN,   // TODO: add other types of security
      info.user,
      info.password);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    throw(connection_error("failed to login to broker: " + to_string(reply)));
  }

  // open channels
  {
    const auto ok = amqp_channel_open(conn, CHANNEL_ID);
    if (!ok) {
      throw(connection_error("failed to open channel (client): " + std::to_string(CHANNEL_ID)));
    }
    throw_on_reply_error(conn, "failed to open channel (broker): ");
  }
  {
    const auto ok = amqp_channel_open(conn, CONFIRMING_CHANNEL_ID);
    if (!ok) {
      throw(connection_error("failed to open channel (client): " + std::to_string(CONFIRMING_CHANNEL_ID)));
    }
    throw_on_reply_error(conn, "failed to open channel (broker): ");
  }
  {
    const auto ok = amqp_confirm_select(conn, CONFIRMING_CHANNEL_ID);
    if (!ok) {
      throw(connection_error("failed to set channel to 'confirm' mode (client): " + std::to_string(CONFIRMING_CHANNEL_ID)));
    }
    throw_on_reply_error(conn, "failed to set channel to 'confirm' mode (broker): ");
  }

  // verify that the topic exchange is there
  {
    const auto ok = amqp_exchange_declare(conn, 
      CHANNEL_ID,
      amqp_cstring_bytes(exchange.c_str()),
      amqp_cstring_bytes("topic"),
      1, // passive - exchange must already exist on broker
      1, // durable
      0, // dont auto-delete
      0, // not internal
      amqp_empty_table);
    if (!ok) {
      throw(connection_error("could not query exchange"));
    }
    throw_on_reply_error(conn, "exchange not found: ");
  }
  {
    // create queue for confirmations
    const auto queue_ok  = amqp_queue_declare(conn, 
        CHANNEL_ID,         // use the regular channel for this call
        amqp_empty_bytes,   // let broker allocate queue name
        0,                  // not passive - create the queue 
        0,                  // not durable 
        1,                  // exclusive 
        1,                  // auto-delete
        amqp_empty_table    // not args TODO add args from conf: TTL, max length etc.
        );
    if (!queue_ok) {
      throw(connection_error("failed to create queue for incomming confimations (client)"));
    }
    auto reply = amqp_get_rpc_reply(conn);
    if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
      throw(connection_error("failed to create queue for incomming confimations (broker):" + to_string(reply)));
    }

    // define consumption for connection
    const auto consume_ok = amqp_basic_consume(conn, 
        CONFIRMING_CHANNEL_ID, 
        queue_ok->queue,
        amqp_empty_bytes, // broker will generate consumer tag
        1,                // messages sent from client are never routed back
        1,                // client does not ack thr acks
        1,                // exclusive access to queue
        amqp_empty_table  // no parameters
        );

    if (!consume_ok) {
      throw(connection_error("failed to define message consumption (client)"));
    }
    reply = amqp_get_rpc_reply(conn);
    if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
      throw(connection_error("failed to define message consumption (broker):" + to_string(reply)));
    }
    //TODO broker generated consumer_tag could be used to cancel sending of n/acks from broker - probably not needed

    guard.reset();
    return connection_t(conn, exchange, queue_ok->queue);
  }
}

/// struct used for holding mesages in the message queue
struct message_wrapper_t {
  connection_t& conn; 
  std::string topic;
  std::string message;
  reply_callback_t cb;
  
  message_wrapper_t(connection_t& _conn,
      const std::string& _topic,
      const std::string& _message,
      reply_callback_t _cb) : conn(_conn), topic(_topic), message(_message), cb(_cb) {}
};

// struct for holding the callback and its tag in the callback list
struct reply_callback_with_tag_t {
  uint64_t tag;
  reply_callback_t cb;
  
  reply_callback_with_tag_t(uint64_t _tag, reply_callback_t _cb) : tag(_tag), cb(_cb) {}
  
  bool operator==(uint64_t rhs) {
    return tag == rhs;
  }
};

typedef std::unordered_map<connection_id_t, connection_t, connection_id_t::hasher> ConnectionList;
typedef std::vector<reply_callback_with_tag_t> CallbackList;
typedef boost::lockfree::queue<message_wrapper_t*, boost::lockfree::fixed_sized<true>> MessageQueue;

class Manager {
public:
  const size_t max_connections;
  const size_t max_inflight;
  const size_t max_queue;
private:
  size_t connection_number;
  std::atomic<bool> stopped;
  struct timeval read_timeout;
  ConnectionList connections;
  CallbackList callbacks;
  MessageQueue messages;
  size_t queued;
  size_t dequeued;
  std::mutex connections_lock;
  std::thread runner;

  void publish_internal(message_wrapper_t* message) {
    const std::unique_ptr<message_wrapper_t> msg_owner(message);
    if (!message->conn.state) {
      // connection was deleted while message was in the queue
      // TODO add error stats
      if (message->cb) {
        message->cb(RGW_AMQP_STATUS_CONNECTION_CLOSED);
      }
      return;
    }

    if (message->cb == nullptr) {
      // TODO add error stats
      amqp_basic_publish(message->conn.state,
        CHANNEL_ID,
        amqp_cstring_bytes(message->conn.exchange.c_str()),
        amqp_cstring_bytes(message->topic.c_str()),
        1, // mandatory, TODO: take from conf
        0, // not immediate
        nullptr,
        amqp_cstring_bytes(message->message.c_str()));
        return;
    }

    amqp_basic_properties_t props;
    props._flags = 
      AMQP_BASIC_DELIVERY_MODE_FLAG | 
      AMQP_BASIC_REPLY_TO_FLAG;
    props.delivery_mode = 2; // persistent delivery TODO take from conf
    props.reply_to = message->conn.reply_to_queue;

    const auto rc = amqp_basic_publish(message->conn.state,
      CONFIRMING_CHANNEL_ID,
      amqp_cstring_bytes(message->conn.exchange.c_str()),
      amqp_cstring_bytes(message->topic.c_str()),
      1, // mandatory, TODO: take from conf
      0, // not immediate
      &props,
      amqp_cstring_bytes(message->message.c_str()));

    if (rc == AMQP_STATUS_OK) {
      if (callbacks.size() < max_inflight) {
        callbacks.emplace_back(message->conn.delivery_tag++, message->cb);
      } else {
        // immediatly invoke callback with error
        message->cb(RGW_AMQP_STATUS_MAX_INFLIGHT);
      }
    } else {
      // immediatly invoke callback with error
      message->cb(rc);
    }
  }

  // the managers thread:
  // (1) empty the queue of messages to be published
  // (2) loop over all connections and read acks
  // (3) manages deleted connections
  // (4) TODO reconnect on conection errors
  void run() {
    amqp_frame_t frame;
    //PublishInternal publish_internal_func(this);
    while (!stopped) {

      // publish all messages in the queue
      //const auto count = messages.consume_all(publish_internal_func);
      const auto count = messages.consume_all(std::bind(&Manager::publish_internal, this, std::placeholders::_1));
      dequeued += count;
      ConnectionList::iterator conn_it;
      ConnectionList::const_iterator end_it;
      {
        // thread safe access to the connection list
        // once the iterators are fetched they are guaranteed to remain valid
        std::lock_guard<std::mutex> lock(connections_lock);
        conn_it = connections.begin();
        end_it = connections.end();
      }
      auto incoming_message = false;
      // loop over all connections to read acks
      for (;conn_it != end_it;) {
        if (conn_it->second.mark_for_deletion) {
          conn_it->second.destroy();
          std::lock_guard<std::mutex> lock(connections_lock);
          // erase is safe - does not invalidate any other iterator
          // lock so no insertion happens at the same time
          conn_it = connections.erase(conn_it);
          --connection_number;
          continue;
        }
        const auto rc = amqp_simple_wait_frame_noblock(conn_it->second.state, &frame, &read_timeout);

        // iterator not deleted - increment it
        // note: the iterator should not be used from now on inside the loop
        ++conn_it;

        if (rc == AMQP_STATUS_TIMEOUT) {
          // TODO mark connection as idle
          continue;
        }
       
        // this is just to prevent spinning idle, does not indicate that a message
        // was sucessfully processd or not
        incoming_message = true;
        if (rc != AMQP_STATUS_OK) {
          // TODO error handling, add a counter
          continue;
        }

        if (frame.frame_type != AMQP_FRAME_METHOD) {
          // handler is for publish confirmation only - ignore all other messages
          // TODO: add a counter
          continue;
        }

        uint64_t tag;
        bool multiple;
        int result;

        if (frame.payload.method.id == AMQP_BASIC_ACK_METHOD) {
          result = AMQP_STATUS_OK;
          const auto ack = (amqp_basic_ack_t*)frame.payload.method.decoded;
          ceph_assert(ack);
          tag = ack->delivery_tag;
          multiple = ack->multiple;
        } else if (frame.payload.method.id == AMQP_BASIC_NACK_METHOD) {
          result = RGW_AMQP_STATUS_BROKER_NACK;
          const auto nack = (amqp_basic_nack_t*)frame.payload.method.decoded;
          ceph_assert(nack);
          tag = nack->delivery_tag;
          multiple = nack->multiple;
        } else {
          // unexpected method
          // TODO: add a counter
          continue;
        }

        const auto it = std::find(callbacks.begin(), callbacks.end(), tag);
        if (it != callbacks.end()) {
          if (multiple) {
            // n/ack all up to (and including) the tag
            for (auto rit = it; rit >= callbacks.begin(); --rit) {
              rit->cb(result);
              callbacks.erase(rit);
            }
          } else {
            // n/ack a specific tag
            it->cb(result);
            callbacks.erase(it);
          }
        } else {
          // TODO add counter for acks with no callback
        }
      }
      // if no messages were received or published, sleep for 100ms
      if (count == 0 && !incoming_message) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  }

  // used in the dtor for message cleanup
  static void delete_message(const message_wrapper_t* message) {
    delete message;
  }

public:
  Manager(size_t _max_connections,
      size_t _max_inflight,
      size_t _max_queue, 
      long _usec_timeout) : 
    max_connections(_max_connections),
    max_inflight(_max_inflight),
    max_queue(_max_queue),
    connection_number(0),
    stopped(false),
    read_timeout{0, _usec_timeout},
    connections(_max_connections),
    messages(max_queue),
    queued(0),
    dequeued(0),
    runner(&Manager::run, this) {
      // The hashmap has "max connections" as the initial number of buckets, 
      // and allows for 10 collisions per bucket before rehash.
      // This is to prevent rehashing so that iterators are not invalidated 
      // when a new connection is added.
      connections.max_load_factor(10.0);
  }

  // non copyable
  Manager(const Manager&) = delete;
  const Manager& operator=(const Manager&) = delete;

  // stop the main thread
  void stop() {
    stopped = true;
  }

  // disconnect from a broker
  bool disconnect(connection_t& conn) {
    if (stopped) {
      return false;
    }
    conn.mark_for_deletion = true;
    return true;
  }

  // connect to a broker, or reuse an existing connection if already connected
  connection_t& connect(const std::string& url, const std::string& exchange) {
    if (stopped) {
      throw(connection_error("manager is stopped"));
    }

    struct amqp_connection_info info;
    // cache the URL so that parsing could happen in-place
    std::vector<char> url_cache(url.c_str(), url.c_str()+url.size()+1);
    if (AMQP_STATUS_OK != amqp_parse_url(url_cache.data(), &info)) {
      throw(connection_error("failed to parse URL: " + url));
    }

    const connection_id_t id(info);
    std::lock_guard<std::mutex> lock(connections_lock);
    const auto it = connections.find(id);
    if (it != connections.end()) {
      if (it->second.mark_for_deletion) {
        throw(connection_error("connection in process of deletion"));
      } else if (it->second.exchange != exchange) {
        throw(connection_error("exchange mismatch: " + it->second.exchange + " != " + exchange));
      }
      // connection found
      return it->second;
    }

    // connection not found, creating a new one
    if (connection_number >= max_connections) {
      throw(connection_error("max connections reached: " + std::to_string(max_connections)));
    }
    const auto conn = create_connection(info, exchange);
    // if connection creation fails, it will throw and exception
    ++connection_number;
    return connections.emplace(id, conn).first->second;
  }

  // TODO publish with confirm is needed in "none" case as well, cb should be invoked publish is ok (no ack)
  int publish(connection_t& conn, 
    const std::string& topic,
    const std::string& message) {
    if (!conn.state || conn.mark_for_deletion) {
      return RGW_AMQP_STATUS_CONNECTION_CLOSED;
    }
    if (messages.push(new message_wrapper_t(conn, topic, message, nullptr))) {
      ++queued;
      return AMQP_STATUS_OK;
    }
    return RGW_AMQP_STATUS_QUEUE_FULL;
  }
  
  int publish_with_confirm(connection_t& conn, 
    const std::string& topic,
    const std::string& message,
    reply_callback_t cb) {
    if (!conn.state || conn.mark_for_deletion) {
      return RGW_AMQP_STATUS_CONNECTION_CLOSED;
    }
    if (messages.push(new message_wrapper_t(conn, topic, message, cb))) {
      ++queued;
      return AMQP_STATUS_OK;
    }
    return RGW_AMQP_STATUS_QUEUE_FULL;
  }

  // dtor wait for thread to stop
  // then connection are cleaned-up
  ~Manager() {
    stopped = true;
    runner.join();
    messages.consume_all(delete_message);
    for (auto& conn : connections) {
      conn.second.destroy();
    }
  }

  // get the number of connections
  size_t get_connection_number() const {
    return connection_number;
  }
  
  // get the number of in-flight messages
  size_t get_inflight() const {
    return callbacks.size();
  }

  // running counter of the queued messages
  size_t get_queued() const {
    return queued;
  }

  // running counter of the dequeued messages
  size_t get_dequeued() const {
    return dequeued;
  }
};

// singleton manager
// note that the manager itself is not a singleton, and multiple instances may co-exist
// TODO get parameters from conf
Manager s_manager(256, 8192, 8192, 100);

connection_t& connect(const std::string& url, const std::string& exchange) {
  return s_manager.connect(url, exchange);
}

int publish(connection_t& conn, 
    const std::string& topic,
    const std::string& message) {
  return s_manager.publish(conn, topic, message);
}

int publish_with_confirm(connection_t& conn, 
    const std::string& topic,
    const std::string& message,
    reply_callback_t cb) {
  return s_manager.publish_with_confirm(conn, topic, message, cb);
}

size_t get_connection_number() {
  return s_manager.get_connection_number();
}
  
size_t get_inflight() {
  return s_manager.get_inflight();
}

size_t get_queued() {
  return s_manager.get_queued();
}

size_t get_dequeued() {
  return s_manager.get_dequeued();
}

size_t get_max_connections() {
  return s_manager.max_connections;
}

size_t get_max_inflight() {
  return s_manager.max_inflight;
}

size_t get_max_queue() {
  return s_manager.max_queue;
}

bool disconnect(connection_t& conn) {
  return s_manager.disconnect(conn);
}

} // namespace amqp

