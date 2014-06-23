// Copyright (c) 2013, Cloudera, inc.

#include "tablet/rowset_tree.h"

#include <algorithm>
#include <string>
#include <vector>

#include "gutil/stl_util.h"
#include "tablet/rowset.h"
#include "util/interval_tree.h"
#include "util/interval_tree-inl.h"
#include "util/slice.h"

using std::vector;
using std::tr1::shared_ptr;

namespace kudu {
namespace tablet {

namespace {

bool RSEndpointBySliceCompare(const RowSetTree::RSEndpoint& a,
                              const RowSetTree::RSEndpoint& b) {
  static const Slice::Comparator comp = Slice::Comparator();
  return comp(a.slice_, b.slice_);
}

}

// Entry for use in the interval tree.
struct RowSetWithBounds {
  RowSet *rowset;
  string min_key;
  string max_key;
};

// Traits struct for IntervalTree.
struct RowSetIntervalTraits {
  typedef Slice point_type;
  typedef RowSetWithBounds *interval_type;

  static Slice get_left(const RowSetWithBounds *rs) {
    return Slice(rs->min_key);
  }

  static Slice get_right(const RowSetWithBounds *rs) {
    return Slice(rs->max_key);
  }

  static int compare(const Slice &a, const Slice &b) {
    return a.compare(b);
  }
};

RowSetTree::RowSetTree()
  : initted_(false) {
}

Status RowSetTree::Reset(const RowSetVector &rowsets) {
  CHECK(!initted_);
  std::vector<RowSetWithBounds *> entries;
  RowSetVector unbounded;
  ElementDeleter deleter(&entries);
  entries.reserve(rowsets.size());
  std::vector<RSEndpoint> endpoints;
  endpoints.reserve(rowsets.size()*2);

  // Iterate over each of the provided RowSets, fetching their
  // bounds and adding them to the local vectors.
  BOOST_FOREACH(const shared_ptr<RowSet> &rs, rowsets) {
    gscoped_ptr<RowSetWithBounds> rsit(new RowSetWithBounds());
    rsit->rowset = rs.get();
    Slice min_key, max_key;
    Status s = rs->GetBounds(&min_key, &max_key);
    if (s.IsNotSupported()) {
      // This rowset is a MemRowSet, for which the bounds change as more
      // data gets inserted. Therefore we can't put it in the static
      // interval tree -- instead put it on the list which is consulted
      // on every access.
      unbounded.push_back(rs);
      continue;
    } else if (!s.ok()) {
      LOG(WARNING) << "Unable to construct RowSetTree: "
                   << rs->ToString() << " unable to determine its bounds: "
                   << s.ToString();
      return s;
    }
    // Load into key endpoints.
    endpoints.push_back(RSEndpoint(rsit->rowset, START, min_key));
    endpoints.push_back(RSEndpoint(rsit->rowset, STOP, max_key));

    // Load bounds and save entry
    rsit->min_key = min_key.ToString();
    rsit->max_key = max_key.ToString();
    entries.push_back(rsit.release());
  }

  // Sort endpoints
  std::sort(endpoints.begin(), endpoints.end(), RSEndpointBySliceCompare);

  // Install the vectors into the object.
  entries_.swap(entries);
  unbounded_rowsets_.swap(unbounded);
  tree_.reset(new IntervalTree<RowSetIntervalTraits>(entries_));
  key_endpoints_.swap(endpoints);
  all_rowsets_.assign(rowsets.begin(), rowsets.end());
  initted_ = true;

  return Status::OK();
}

void RowSetTree::FindRowSetsIntersectingInterval(const Slice &lower_bound,
                                                 const Slice &upper_bound,
                                                 vector<RowSet *> *rowsets) const {
  DCHECK(initted_);

  // All rowsets with unknown bounds need to be checked.
  BOOST_FOREACH(const shared_ptr<RowSet> &rs, unbounded_rowsets_) {
    rowsets->push_back(rs.get());
  }

  // perf TODO: make it possible to query using raw Slices
  // instead of copying to strings here
  RowSetWithBounds query;
  query.min_key = lower_bound.ToString();
  query.max_key = upper_bound.ToString();

  vector<RowSetWithBounds *> from_tree;
  from_tree.reserve(all_rowsets_.size());
  tree_->FindIntersectingInterval(&query, &from_tree);
  rowsets->reserve(rowsets->size() + from_tree.size());
  BOOST_FOREACH(RowSetWithBounds *rs, from_tree) {
    rowsets->push_back(rs->rowset);
  }
}

void RowSetTree::FindRowSetsWithKeyInRange(const Slice &encoded_key,
                                           vector<RowSet *> *rowsets) const {
  DCHECK(initted_);

  // All rowsets with unknown bounds need to be checked.
  BOOST_FOREACH(const shared_ptr<RowSet> &rs, unbounded_rowsets_) {
    rowsets->push_back(rs.get());
  }

  // Query the interval tree to efficiently find rowsets with known bounds
  // whose ranges overlap the probe key.
  vector<RowSetWithBounds *> from_tree;
  from_tree.reserve(all_rowsets_.size());
  tree_->FindContainingPoint(encoded_key, &from_tree);
  rowsets->reserve(rowsets->size() + from_tree.size());
  BOOST_FOREACH(RowSetWithBounds *rs, from_tree) {
    rowsets->push_back(rs->rowset);
  }
}

RowSetTree::~RowSetTree() {
  STLDeleteElements(&entries_);
}

} // namespace tablet
} // namespace kudu
