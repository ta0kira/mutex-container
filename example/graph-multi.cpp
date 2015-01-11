#include <set>
#include <map>
#include <queue>
#include <string>
#include <memory>
#include <cassert>
#include <iostream>

#include <stdio.h>

#include "locking-container.hpp"
//(necessary for non-template source)
#include "locking-container.inc"


typedef lc::lock_auth_base::auth_type auth_type;
typedef lc::shared_multi_lock         shared_multi_lock;


template <class Type>
struct graph_node {
  typedef Type stored_type;
  typedef lc::locking_container_base <graph_node> protected_node;
  typedef std::shared_ptr <protected_node>        shared_node;
  typedef std::set <shared_node>                  connected_nodes;

  inline graph_node(const stored_type &value) : obj(value) {}

  inline graph_node(stored_type &&value = stored_type()) : obj(std::move(value)) {}

  inline graph_node(graph_node &&other) : out(std::move(other.out)),
    in(std::move(other.in)), obj(std::move(other.obj)) {}

private:
  graph_node(const graph_node&);
  graph_node &operator = (const graph_node&);

public:
  static inline bool connect_nodes(shared_node left, shared_node right,
   auth_type auth = auth_type(), shared_multi_lock master_lock = shared_multi_lock(),
   bool try_multi = true) {
    return change_connection_common(&insert_edge, left, right, auth, master_lock, try_multi);
  }

  static inline bool disconnect_nodes(shared_node left, shared_node right,
   auth_type auth = auth_type(), shared_multi_lock master_lock = shared_multi_lock(),
   bool try_multi = true) {
    return change_connection_common(&erase_edge, left, right, auth, master_lock, try_multi);
  }

  virtual inline ~graph_node() {}

  connected_nodes out, in;
  stored_type     obj;

protected:
  static void insert_edge(connected_nodes &left, const shared_node &right) {
    left.insert(right);
  }

  static void erase_edge(connected_nodes &left, const shared_node &right) {
    left.erase(right);
  }

  static void get_two_writes(shared_node left, shared_node right, auth_type auth,
    shared_multi_lock master_lock, typename protected_node::write_proxy &write1,
    typename protected_node::write_proxy &write2, bool block = true) {
    assert(left.get() && right.get());
    bool order = left->get_order() < right->get_order();
    if (master_lock && auth) {
      if (order) {
        write1 = left->get_write_multi(*master_lock, auth, block);
        write2 = right->get_write_multi(*master_lock, auth, block);
      } else {
        write2 = right->get_write_multi(*master_lock, auth, block);
        write1 = left->get_write_multi(*master_lock, auth, block);
      }
    } else if (auth) {
      if (order) {
        write1 = left->get_write_auth(auth, block);
        write2 = right->get_write_auth(auth, block);
      } else {
        write2 = right->get_write_auth(auth, block);
        write1 = left->get_write_auth(auth, block);
      }
    } else {
      write1 = left->get_write(block);
      write2 = right->get_write(block);
    }
  }

  template <class Func>
  static bool change_connection_common(Func func, shared_node left, shared_node right,
    auth_type auth = auth_type(), shared_multi_lock master_lock = shared_multi_lock(),
    bool try_multi = true) {
    lc::multi_lock::write_proxy multi;
    if (try_multi && master_lock && !(multi = master_lock->get_write_auth(auth))) return false;

    typename protected_node::write_proxy write_l, write_r;
    get_two_writes(left, right, auth, master_lock, write_l, write_r);
    multi.clear();
    if (!write_l && !write_r) return false;

    (*func)(write_l->out, right);
    (*func)(write_r->in,  left);

    return true;
  }
};


template <class Type>
struct graph_head {
  typedef graph_node <Type>              node;
  typedef typename node::stored_type     stored_type;
  typedef typename node::protected_node  protected_node;
  typedef typename node::shared_node     shared_node;

  virtual shared_node get_graph_head() = 0;

  virtual typename lc::multi_lock_base::write_proxy get_master_lock(auth_type auth) = 0;
  virtual lc::multi_lock_base &show_master_lock() = 0;

  virtual inline ~graph_head() {}
};


template <class Index, class Type>
class graph : public graph_head <Type> {
public:
  typedef graph_head <Type> base;
  using typename base::node;
  using typename base::stored_type;
  using typename base::protected_node;
  using typename base::shared_node;
  typedef Index                              index_type;
  typedef std::map <index_type, shared_node> node_map;

  graph() : master_lock(new lc::multi_lock) {}

private:
  graph(const graph&);
  graph &operator = (const graph&);

public:
  shared_node get_graph_head() {
    return all_nodes.size()? all_nodes.begin()->second : shared_node();
  }

  typename lc::multi_lock_base::write_proxy get_master_lock(auth_type auth) {
    return master_lock? master_lock->get_write_auth(auth) : lc::multi_lock_base::write_proxy();
  }

  typename lc::multi_lock_base &show_master_lock() {
    assert(master_lock.get());
    return *master_lock;
  }

  virtual bool connect_nodes(shared_node left, shared_node right, auth_type auth) {
    //NOTE: this doesn't use 'find_node' so that error returns only pertain to
    //failed lock operations
    return node::connect_nodes(left, right, auth, master_lock);
  }
  virtual bool disconnect_nodes(shared_node left, shared_node right, auth_type auth) {
    //NOTE: this doesn't use 'find_node' so that error returns only pertain to
    //failed lock operations
    return node::disconnect_nodes(left, right, auth, master_lock);
  }

  virtual shared_node find_node(const index_type &index) {
    //NOTE: this doesn't have side-effects!
    typename node_map::iterator found = all_nodes.find(index);
    //NOTE: the line below must not have side-effects!
    return (found == all_nodes.end())? shared_node() : found->second;
  }

  virtual bool insert_node(const index_type &index, shared_node value, auth_type auth) {
    assert(value.get());
    return this->change_node(index, auth, &replace_node, value);
  }

  virtual bool erase_node(const index_type &index, shared_node value, auth_type auth) {
    return this->change_node(index, auth, &remove_node);
  }

  virtual ~graph() {
    for (typename node_map::iterator current = all_nodes.begin(), end = all_nodes.end();
         current != end; ++current) {
      //NOTE: if it's already locked, that's a serious problem here
      typename node::protected_node::write_proxy write = current->second->get_write(false);
      assert(write);
      write->out.clear();
      write->in.clear();
    }
  }

protected:
  static void replace_node(node_map &all_nodes, const index_type &index, shared_node value) {
    all_nodes[index] = value;
  }

  static void remove_node(node_map &all_nodes, const index_type &index) {
    all_nodes.erase(index);
  }

  template <class Member>
  bool remove_edges(shared_node value, Member remove_left, Member remove_right, auth_type auth) {
    assert(master_lock.get());
    typename node::protected_node::write_proxy left = value->get_write_multi(*master_lock, auth);
    if (!left) return false;
    for (typename node::connected_nodes::iterator
         current = (left->*remove_left).begin(), end = (left->*remove_left).end();
         current != end; ++current) {
      assert(current->get());
      typename node::protected_node::write_proxy right = (*current)->get_write_multi(*master_lock, auth);
      if (!right) return false;
      (right->*remove_right).erase(value);
    }
    return true;
  }

  template <class Func, class ... Args>
  bool change_node(const index_type &index, auth_type auth, Func func, Args ... args) {
    assert(master_lock.get());
    lc::multi_lock::write_proxy multi = master_lock->get_write_auth(auth);
    if (!multi) return false;
    shared_node old_node = this->find_node(index);
    if (old_node) {
      //NOTE: these should never fail if 'master_lock' is used properly
      if (!this->remove_edges(old_node, &node::out, &node::in, auth)) return false;
      if (!this->remove_edges(old_node, &node::in, &node::out, auth)) return false;
    }
    //NOTE: if this results in destruction of the old node, it shouldn't have
    //any locks on it that will cause problems
    (*func)(all_nodes, index, args...);
    return true;
  }

private:
  shared_multi_lock master_lock;
  node_map          all_nodes;
};


template <class Type>
static const Type &identity(const Type &value) {
  return value;
}


template <class Type, class Result = const Type&>
static bool print_graph(graph_head <Type> &the_graph, auth_type auth,
  Result(*convert)(const Type&) = &identity <Type>) {
  typedef graph_head <Type> graph_type;
  typedef std::queue <typename graph_type::protected_node::write_proxy> proxy_queue;
  proxy_queue locked, pending;

  lc::multi_lock_base::write_proxy multi = the_graph.get_master_lock(auth);
  if (!multi) return false;

  typename graph_type::shared_node head = the_graph.get_graph_head();
  if (!head) return true;

  typename graph_type::protected_node::write_proxy next =
    head->get_write_multi(the_graph.show_master_lock(), auth);
  //(nothing should be locked at this point)
  if (!next) return false;
  locked.push(next);

  std::cout << (*convert)(next->obj) << " (first node)" << std::endl;

  while (next) {
    for (typename graph_type::node::connected_nodes::iterator
         current = next->out.begin(), end = next->out.end();
         current != end; ++current) {
      assert(current->get());
      typename graph_type::protected_node::write_proxy write =
        (*current)->get_write_multi(the_graph.show_master_lock(), auth);
      //NOTE: this should only happen if we already have the lock
      if (!write) continue;

      std::cout << (*convert)(write->obj) << " (first seen from "
                << (*convert)(next->obj)  << ")" << std::endl;

      pending.push(write);
      locked.push(write);
    }

    next = pending.size()? pending.front() : typename graph_type::protected_node::write_proxy();
    if (next) pending.pop();
  }

  return true;
}


struct tagged_value {
  tagged_value(int t, int v = 0) : tag(t), value(v) {}

  static int get_tag(const tagged_value &object) {
    return object.tag;
  }

  const int tag;
  int       value;
};


typedef graph <int, tagged_value> int_graph;
typedef lc::locking_container <int_graph::node, lc::w_lock> locking_node;


int main() {
  int_graph main_graph;

  auth_type main_auth(locking_node::new_auth());

  for (int i = 0; i < 10; i++) {
    if (!main_graph.insert_node(i, int_graph::shared_node(new locking_node(tagged_value(i))), main_auth)) {
      fprintf(stderr, "could not add node %i\n", i);
      return 1;
    } else {
      fprintf(stderr, "added node %i\n", i);
    }
  }

  for (int i = 0; i < 10; i++) {
    int from = i, to = (i + 1) % 10;
    int_graph::shared_node left  = main_graph.find_node(from);
    int_graph::shared_node right = main_graph.find_node(to);
    if (!left || !right) {
      fprintf(stderr, "error finding nodes\n");
      return 1;
    }
    if (!main_graph.connect_nodes(left, right, main_auth)) {
      fprintf(stderr, "could not connect node %i to %i\n", from, to);
      return 1;
    } else {
      fprintf(stderr, "connected node %i to %i\n", from, to);
    }
  }

  print_graph(main_graph, main_auth, &tagged_value::get_tag);
}
