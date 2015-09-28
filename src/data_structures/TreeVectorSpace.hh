/* -*-  mode: c++; c-default-style: "google"; indent-tabs-mode: nil -*- */
/* -------------------------------------------------------------------------
   ATS

   License: see $ATS_DIR/COPYRIGHT
   Author: Ethan Coon

   Space for a TreeVector on an Amanzi mesh.
   ------------------------------------------------------------------------- */

#ifndef AMANZI_TREEVECTOR_SPACE_HH_
#define AMANZI_TREEVECTOR_SPACE_HH_

#include <vector>
#include "Teuchos_RCP.hpp"
#include "Iterators.hh"
#include "Mesh.hh"

namespace Amanzi {

class CompositeVectorSpace;

class TreeVectorSpace {

 public:
  // Constructor
  TreeVectorSpace() {}
  TreeVectorSpace(const Teuchos::RCP<const CompositeVectorSpace>& cvfac) :
      data_(cvfac) {}
  TreeVectorSpace(const TreeVectorSpace& other);

  // Checks equality
  bool SameAs(const TreeVectorSpace& other) const;

  // Set/get data space
  Teuchos::RCP<const CompositeVectorSpace> Data() const { return data_; }
  void SetData(const Teuchos::RCP<const CompositeVectorSpace>& data) {
    data_ = data; }


  // Access to SubVectors
  typedef std::vector<Teuchos::RCP<const TreeVectorSpace> > SubVectorsContainer;
  typedef Utils::iterator<SubVectorsContainer, const TreeVectorSpace> iterator;
  typedef Utils::const_iterator<SubVectorsContainer, const TreeVectorSpace> const_iterator;

  const_iterator begin() const { return const_iterator(subvecs_.begin()); }
  const_iterator end() const { return const_iterator(subvecs_.end()); }
  size_t size() const { return subvecs_.size(); }

  // Get a pointer to the sub-vector by index
  Teuchos::RCP<const TreeVectorSpace> SubVector(int index) const;

  // Add a sub-vector as a child of this node.
  void PushBack(const Teuchos::RCP<const TreeVectorSpace>& subvec);

 private:
  // private and unimplemented
  TreeVectorSpace& operator=(const TreeVectorSpace&);

 private:
  Teuchos::RCP<const CompositeVectorSpace> data_;
  std::vector< Teuchos::RCP<const TreeVectorSpace> > subvecs_;

};

} // namespace amanzi

#endif
