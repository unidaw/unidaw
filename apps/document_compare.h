#pragma once

// DID THIS COMMAND CHANGE ANYTHING THE USER AUTHORED?
//
// The first real consumer of the field visitor, and the reason the walk names fields by MEMBER
// POINTER: one description of a field, applied to TWO objects at once. The earlier signature
// (`visitFields(T& value, V&)`) could serialize and hash but could not do this, which is how the
// design flaw was found — by trying to write this file.
//
// WHAT IT REPLACES. DocumentHistory::commit currently answers this question by serializing the
// whole document to JSON and comparing strings. That is CORRECT — it is why a refused command no
// longer destroys the redo tail — but it builds megabytes of text per mutating command, and a knob
// drag emits one command per milli-unit (#119 item 10). This walks the fields and stops at the
// first difference.
//
// IT ANSWERS YES/NO, NOT "WHAT CHANGED". A differ needs identity-keyed matching of vectors so it
// can say "device 7 changed" rather than "the third element differs", and chains are
// order-significant so it must also distinguish CHANGED from MOVED. That is the next step;
// equality is the half undo needs today, and shipping it first keeps the change reviewable.
//
// DERIVED AND SESSION FIELDS ARE SKIPPED, which is the entire reason FieldKind exists.
// hostSlotIndex is an index into this machine's plugin scan — comparing it would report a change
// whenever the cache differed, on a document nobody touched. Session fields are skipped for the
// same reason undo must not restore them (53b77d5: undo used to wipe the loop region).

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "apps/document_visitor_fields.h"

namespace daw {

template <typename T>
bool documentFieldsEqual(const T& a, const T& b);

namespace detail {

// Does this type declare a field list? Probed with a visitor that does nothing, so the answer is
// "is there a visitFields_ overload for it", independent of what any real visitor needs.
struct ProbeVisitor {
  template <typename C, typename M>
  void field(const char*, M C::*, FieldKind = FieldKind::Authored) {}
};

template <typename U, typename = void>
struct HasFieldList : std::false_type {};
template <typename U>
struct HasFieldList<
    U, std::void_t<decltype(visitFields_(TypeTag<U>{}, std::declval<ProbeVisitor&>()))>>
    : std::true_type {};

// Two objects, one field list, walked in lockstep.
template <typename T>
struct EqualityVisitor {
  const T& a;
  const T& b;
  bool equal = true;

  template <typename C, typename M>
  void field(const char* name, M C::*member, FieldKind kind = FieldKind::Authored) {
    (void)name;
    if (!equal) {
      return;  // already answered; every later field is wasted work
    }
    if (kind == FieldKind::Derived || kind == FieldKind::Session) {
      return;
    }
    if (!valueEqual(a.*member, b.*member)) {
      equal = false;
    }
  }

  // A type WITH A FIELD LIST recurses into it; anything else uses operator==. The test is the
  // EXISTENCE OF A LIST, not whether some template happens to compile — an earlier version keyed
  // off `decltype(documentFieldsEqual(x, y))`, which is declared for every type and therefore
  // matched uint32_t as readily as ProjectTrack, producing an ambiguous overload on the first
  // field it saw. Keying off the list means a struct that gains fields but no list is compared by
  // its own operator==, and one with neither fails to compile — never silently "equal", which is
  // the failure mode this whole mechanism exists to prevent.
  template <typename U>
  static bool valueEqual(const std::vector<U>& x, const std::vector<U>& y) {
    if (x.size() != y.size()) {
      return false;
    }
    for (size_t i = 0; i < x.size(); ++i) {
      if (!valueEqual(x[i], y[i])) {
        return false;
      }
    }
    return true;
  }

  template <typename U>
  static bool valueEqual(const U& x, const U& y) {
    if constexpr (HasFieldList<U>::value) {
      return documentFieldsEqual(x, y);
    } else {
      return x == y;
    }
  }
};

}  // namespace detail

// True when a and b agree on every AUTHORED field, recursively.
template <typename T>
bool documentFieldsEqual(const T& a, const T& b) {
  detail::EqualityVisitor<T> visitor{a, b};
  visitFields<T>(visitor);
  return visitor.equal;
}

// WHICH FIELD, not just whether. Empty when the two are equal.
//
// The yes/no answer is what commit() needs and it is a poor diagnostic: "the document changed"
// sends a reader to diff two megabytes of JSON. This walks the same lists and names the first
// field that differs, one level deep — enough to say "device_chain" or "clips" and then be run
// again on that member. Not part of the equality path, so it costs nothing when nobody asks.
namespace detail {

template <typename T>
struct FirstDifferenceVisitor {
  const T& a;
  const T& b;
  std::string found;

  template <typename C, typename M>
  void field(const char* name, M C::*member, FieldKind kind = FieldKind::Authored) {
    if (!found.empty() || kind == FieldKind::Derived || kind == FieldKind::Session) {
      return;
    }
    if (!EqualityVisitor<T>::valueEqual(a.*member, b.*member)) {
      found = name;
    }
  }
};

}  // namespace detail

template <typename T>
std::string documentFirstDifference(const T& a, const T& b) {
  detail::FirstDifferenceVisitor<T> visitor{a, b, {}};
  visitFields<T>(visitor);
  return visitor.found;
}

}  // namespace daw
